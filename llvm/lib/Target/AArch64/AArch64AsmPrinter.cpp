//===- AArch64AsmPrinter.cpp - AArch64 LLVM assembly writer ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to the AArch64 assembly language.
//
//===----------------------------------------------------------------------===//

#include "AArch64.h"
#include "AArch64MCInstLower.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64RegisterInfo.h"
#include "AArch64Subtarget.h"
#include "AArch64TargetObjectFile.h"
#include "MCTargetDesc/AArch64AddressingModes.h"
#include "MCTargetDesc/AArch64InstPrinter.h"
#include "MCTargetDesc/AArch64MCExpr.h"
#include "MCTargetDesc/AArch64MCTargetDesc.h"
#include "MCTargetDesc/AArch64TargetStreamer.h"
#include "TargetInfo/AArch64TargetInfo.h"
#include "Utils/AArch64BaseInfo.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/FaultMaps.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/StackMaps.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/GlobalPtrAuthInfo.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Instrumentation/HWAddressSanitizer.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>

using namespace llvm;

enum PtrauthCheckMode { Default, Unchecked, Poison, Trap };
static cl::opt<PtrauthCheckMode>
PtrauthAuthChecks("aarch64-ptrauth-auth-checks", cl::Hidden,
                  cl::values(
                    clEnumValN(Unchecked, "none", "don't test for failure"),
                    clEnumValN(Poison, "poison", "poison on failure"),
                    clEnumValN(Trap, "trap", "trap on failure")),
                  cl::desc("Check pointer authentication auth/resign failures"),
                  cl::init(Default));

#define DEBUG_TYPE "asm-printer"

namespace {

class AArch64AsmPrinter : public AsmPrinter {
  AArch64MCInstLower MCInstLowering;
  FaultMaps FM;
  const AArch64Subtarget *STI;
  bool ShouldEmitWeakSwiftAsyncExtendedFramePointerFlags = false;

public:
  AArch64AsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer)), MCInstLowering(OutContext, *this),
        FM(*this) {}

  StringRef getPassName() const override { return "AArch64 Assembly Printer"; }

  /// Wrapper for MCInstLowering.lowerOperand() for the
  /// tblgen'erated pseudo lowering.
  bool lowerOperand(const MachineOperand &MO, MCOperand &MCOp) const {
    return MCInstLowering.lowerOperand(MO, MCOp);
  }

  const MCExpr *
  lowerPtrAuthGlobalConstant(const GlobalPtrAuthInfo &PAI) override;

  void emitStartOfAsmFile(Module &M) override;
  void emitJumpTableInfo() override;
  std::tuple<const MCSymbol *, uint64_t, const MCSymbol *,
             codeview::JumpTableEntrySize>
  getCodeViewJumpTableInfo(int JTI, const MachineInstr *BranchInstr,
                           const MCSymbol *BranchLabel) const override;

  void emitFunctionEntryLabel() override;

  void LowerJumpTableDest(MCStreamer &OutStreamer, const MachineInstr &MI);

  void LowerMOPS(MCStreamer &OutStreamer, const MachineInstr &MI);

  void LowerSTACKMAP(MCStreamer &OutStreamer, StackMaps &SM,
                     const MachineInstr &MI);
  void LowerPATCHPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                       const MachineInstr &MI);
  void LowerSTATEPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                       const MachineInstr &MI);
  void LowerFAULTING_OP(const MachineInstr &MI);

  void LowerPATCHABLE_FUNCTION_ENTER(const MachineInstr &MI);
  void LowerPATCHABLE_FUNCTION_EXIT(const MachineInstr &MI);
  void LowerPATCHABLE_TAIL_CALL(const MachineInstr &MI);
  void LowerPATCHABLE_EVENT_CALL(const MachineInstr &MI, bool Typed);

  typedef std::tuple<unsigned, bool, uint32_t> HwasanMemaccessTuple;
  std::map<HwasanMemaccessTuple, MCSymbol *> HwasanMemaccessSymbols;
  void LowerKCFI_CHECK(const MachineInstr &MI);
  void LowerHWASAN_CHECK_MEMACCESS(const MachineInstr &MI);
  void emitHwasanMemaccessSymbols(Module &M);

  void emitSled(const MachineInstr &MI, SledKind Kind);

  /// tblgen'erated driver function for lowering simple MI->MC
  /// pseudo instructions.
  bool emitPseudoExpansionLowering(MCStreamer &OutStreamer,
                                   const MachineInstr *MI);

  void emitInstruction(const MachineInstr *MI) override;

  void emitFunctionHeaderComment() override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AsmPrinter::getAnalysisUsage(AU);
    AU.setPreservesAll();
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    AArch64FI = MF.getInfo<AArch64FunctionInfo>();
    STI = &MF.getSubtarget<AArch64Subtarget>();

    SetupMachineFunction(MF);

    if (STI->isTargetCOFF()) {
      bool Local = MF.getFunction().hasLocalLinkage();
      COFF::SymbolStorageClass Scl =
          Local ? COFF::IMAGE_SYM_CLASS_STATIC : COFF::IMAGE_SYM_CLASS_EXTERNAL;
      int Type =
        COFF::IMAGE_SYM_DTYPE_FUNCTION << COFF::SCT_COMPLEX_TYPE_SHIFT;

      OutStreamer->beginCOFFSymbolDef(CurrentFnSym);
      OutStreamer->emitCOFFSymbolStorageClass(Scl);
      OutStreamer->emitCOFFSymbolType(Type);
      OutStreamer->endCOFFSymbolDef();
    }

    // Emit the rest of the function body.
    emitFunctionBody();

    // Emit the XRay table for this function.
    emitXRayTable();

    // We didn't modify anything.
    return false;
  }

private:
  void printOperand(const MachineInstr *MI, unsigned OpNum, raw_ostream &O);
  bool printAsmMRegister(const MachineOperand &MO, char Mode, raw_ostream &O);
  bool printAsmRegInClass(const MachineOperand &MO,
                          const TargetRegisterClass *RC, unsigned AltName,
                          raw_ostream &O);

  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNum,
                       const char *ExtraCode, raw_ostream &O) override;
  bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNum,
                             const char *ExtraCode, raw_ostream &O) override;

  void PrintDebugValueComment(const MachineInstr *MI, raw_ostream &OS);

  void emitFunctionBodyEnd() override;

  MCSymbol *GetCPISymbol(unsigned CPID) const override;

  void EmitPtrAuthVersion(Module &M);

  void emitEndOfAsmFile(Module &M) override;

  AArch64FunctionInfo *AArch64FI = nullptr;

  /// Emit the LOHs contained in AArch64FI.
  void emitLOHs();

  /// Emit instruction to set float register to zero.
  void emitFMov0(const MachineInstr &MI);

  using MInstToMCSymbol = std::map<const MachineInstr *, MCSymbol *>;

  MInstToMCSymbol LOHInstToLabel;

  bool shouldEmitWeakSwiftAsyncExtendedFramePointerFlags() const override {
    return ShouldEmitWeakSwiftAsyncExtendedFramePointerFlags;
  }

  const MCSubtargetInfo *getIFuncMCSubtargetInfo() const override {
    assert(STI);
    return STI;
  }
  void emitMachOIFuncStubBody(Module &M, const GlobalIFunc &GI,
                              MCSymbol *LazyPointer) override;
  void emitMachOIFuncStubHelperBody(Module &M, const GlobalIFunc &GI,
                                    MCSymbol *LazyPointer) override;
};

} // end anonymous namespace

void AArch64AsmPrinter::emitStartOfAsmFile(Module &M) {
  const Triple &TT = TM.getTargetTriple();

  if (TT.isOSBinFormatCOFF()) {
    // Emit an absolute @feat.00 symbol
    MCSymbol *S = MMI->getContext().getOrCreateSymbol(StringRef("@feat.00"));
    OutStreamer->beginCOFFSymbolDef(S);
    OutStreamer->emitCOFFSymbolStorageClass(COFF::IMAGE_SYM_CLASS_STATIC);
    OutStreamer->emitCOFFSymbolType(COFF::IMAGE_SYM_DTYPE_NULL);
    OutStreamer->endCOFFSymbolDef();
    int64_t Feat00Value = 0;

    if (M.getModuleFlag("cfguard")) {
      // Object is CFG-aware.
      Feat00Value |= COFF::Feat00Flags::GuardCF;
    }

    if (M.getModuleFlag("ehcontguard")) {
      // Object also has EHCont.
      Feat00Value |= COFF::Feat00Flags::GuardEHCont;
    }

    if (M.getModuleFlag("ms-kernel")) {
      // Object is compiled with /kernel.
      Feat00Value |= COFF::Feat00Flags::Kernel;
    }

    OutStreamer->emitSymbolAttribute(S, MCSA_Global);
    OutStreamer->emitAssignment(
        S, MCConstantExpr::create(Feat00Value, MMI->getContext()));
  }

  if (TM.getTargetTriple().isOSBinFormatMachO())
    EmitPtrAuthVersion(M);

  if (!TT.isOSBinFormatELF())
    return;

  // Assemble feature flags that may require creation of a note section.
  unsigned Flags = 0;
  if (const auto *BTE = mdconst::extract_or_null<ConstantInt>(
          M.getModuleFlag("branch-target-enforcement")))
    if (BTE->getZExtValue())
      Flags |= ELF::GNU_PROPERTY_AARCH64_FEATURE_1_BTI;

  if (const auto *Sign = mdconst::extract_or_null<ConstantInt>(
          M.getModuleFlag("sign-return-address")))
    if (Sign->getZExtValue())
      Flags |= ELF::GNU_PROPERTY_AARCH64_FEATURE_1_PAC;

  if (Flags == 0)
    return;

  // Emit a .note.gnu.property section with the flags.
  auto *TS =
      static_cast<AArch64TargetStreamer *>(OutStreamer->getTargetStreamer());
  TS->emitNoteSection(Flags);
}

void AArch64AsmPrinter::emitFunctionHeaderComment() {
  const AArch64FunctionInfo *FI = MF->getInfo<AArch64FunctionInfo>();
  std::optional<std::string> OutlinerString = FI->getOutliningStyle();
  if (OutlinerString != std::nullopt)
    OutStreamer->getCommentOS() << ' ' << OutlinerString;
}

void AArch64AsmPrinter::LowerPATCHABLE_FUNCTION_ENTER(const MachineInstr &MI)
{
  const Function &F = MF->getFunction();
  if (F.hasFnAttribute("patchable-function-entry")) {
    unsigned Num;
    if (F.getFnAttribute("patchable-function-entry")
            .getValueAsString()
            .getAsInteger(10, Num))
      return;
    emitNops(Num);
    return;
  }

  emitSled(MI, SledKind::FUNCTION_ENTER);
}

void AArch64AsmPrinter::LowerPATCHABLE_FUNCTION_EXIT(const MachineInstr &MI) {
  emitSled(MI, SledKind::FUNCTION_EXIT);
}

void AArch64AsmPrinter::LowerPATCHABLE_TAIL_CALL(const MachineInstr &MI) {
  emitSled(MI, SledKind::TAIL_CALL);
}

void AArch64AsmPrinter::emitSled(const MachineInstr &MI, SledKind Kind) {
  static const int8_t NoopsInSledCount = 7;
  // We want to emit the following pattern:
  //
  // .Lxray_sled_N:
  //   ALIGN
  //   B #32
  //   ; 7 NOP instructions (28 bytes)
  // .tmpN
  //
  // We need the 28 bytes (7 instructions) because at runtime, we'd be patching
  // over the full 32 bytes (8 instructions) with the following pattern:
  //
  //   STP X0, X30, [SP, #-16]! ; push X0 and the link register to the stack
  //   LDR W17, #12 ; W17 := function ID
  //   LDR X16,#12 ; X16 := addr of __xray_FunctionEntry or __xray_FunctionExit
  //   BLR X16 ; call the tracing trampoline
  //   ;DATA: 32 bits of function ID
  //   ;DATA: lower 32 bits of the address of the trampoline
  //   ;DATA: higher 32 bits of the address of the trampoline
  //   LDP X0, X30, [SP], #16 ; pop X0 and the link register from the stack
  //
  OutStreamer->emitCodeAlignment(Align(4), &getSubtargetInfo());
  auto CurSled = OutContext.createTempSymbol("xray_sled_", true);
  OutStreamer->emitLabel(CurSled);
  auto Target = OutContext.createTempSymbol();

  // Emit "B #32" instruction, which jumps over the next 28 bytes.
  // The operand has to be the number of 4-byte instructions to jump over,
  // including the current instruction.
  EmitToStreamer(*OutStreamer, MCInstBuilder(AArch64::B).addImm(8));

  for (int8_t I = 0; I < NoopsInSledCount; I++)
    EmitToStreamer(*OutStreamer, MCInstBuilder(AArch64::HINT).addImm(0));

  OutStreamer->emitLabel(Target);
  recordSled(CurSled, MI, Kind, 2);
}

// Emit the following code for Intrinsic::{xray_customevent,xray_typedevent}
// (built-in functions __xray_customevent/__xray_typedevent).
//
// .Lxray_event_sled_N:
//   b 1f
//   save x0 and x1 (and also x2 for TYPED_EVENT_CALL)
//   set up x0 and x1 (and also x2 for TYPED_EVENT_CALL)
//   bl __xray_CustomEvent or __xray_TypedEvent
//   restore x0 and x1 (and also x2 for TYPED_EVENT_CALL)
// 1:
//
// There are 6 instructions for EVENT_CALL and 9 for TYPED_EVENT_CALL.
//
// Then record a sled of kind CUSTOM_EVENT or TYPED_EVENT.
// After patching, b .+N will become a nop.
void AArch64AsmPrinter::LowerPATCHABLE_EVENT_CALL(const MachineInstr &MI,
                                                  bool Typed) {
  auto &O = *OutStreamer;
  MCSymbol *CurSled = OutContext.createTempSymbol("xray_sled_", true);
  O.emitLabel(CurSled);
  MCInst MovX0Op0 = MCInstBuilder(AArch64::ORRXrs)
                        .addReg(AArch64::X0)
                        .addReg(AArch64::XZR)
                        .addReg(MI.getOperand(0).getReg())
                        .addImm(0);
  MCInst MovX1Op1 = MCInstBuilder(AArch64::ORRXrs)
                        .addReg(AArch64::X1)
                        .addReg(AArch64::XZR)
                        .addReg(MI.getOperand(1).getReg())
                        .addImm(0);
  bool MachO = TM.getTargetTriple().isOSBinFormatMachO();
  auto *Sym = MCSymbolRefExpr::create(
      OutContext.getOrCreateSymbol(
          Twine(MachO ? "_" : "") +
          (Typed ? "__xray_TypedEvent" : "__xray_CustomEvent")),
      OutContext);
  if (Typed) {
    O.AddComment("Begin XRay typed event");
    EmitToStreamer(O, MCInstBuilder(AArch64::B).addImm(9));
    EmitToStreamer(O, MCInstBuilder(AArch64::STPXpre)
                          .addReg(AArch64::SP)
                          .addReg(AArch64::X0)
                          .addReg(AArch64::X1)
                          .addReg(AArch64::SP)
                          .addImm(-4));
    EmitToStreamer(O, MCInstBuilder(AArch64::STRXui)
                          .addReg(AArch64::X2)
                          .addReg(AArch64::SP)
                          .addImm(2));
    EmitToStreamer(O, MovX0Op0);
    EmitToStreamer(O, MovX1Op1);
    EmitToStreamer(O, MCInstBuilder(AArch64::ORRXrs)
                          .addReg(AArch64::X2)
                          .addReg(AArch64::XZR)
                          .addReg(MI.getOperand(2).getReg())
                          .addImm(0));
    EmitToStreamer(O, MCInstBuilder(AArch64::BL).addExpr(Sym));
    EmitToStreamer(O, MCInstBuilder(AArch64::LDRXui)
                          .addReg(AArch64::X2)
                          .addReg(AArch64::SP)
                          .addImm(2));
    O.AddComment("End XRay typed event");
    EmitToStreamer(O, MCInstBuilder(AArch64::LDPXpost)
                          .addReg(AArch64::SP)
                          .addReg(AArch64::X0)
                          .addReg(AArch64::X1)
                          .addReg(AArch64::SP)
                          .addImm(4));

    recordSled(CurSled, MI, SledKind::TYPED_EVENT, 2);
  } else {
    O.AddComment("Begin XRay custom event");
    EmitToStreamer(O, MCInstBuilder(AArch64::B).addImm(6));
    EmitToStreamer(O, MCInstBuilder(AArch64::STPXpre)
                          .addReg(AArch64::SP)
                          .addReg(AArch64::X0)
                          .addReg(AArch64::X1)
                          .addReg(AArch64::SP)
                          .addImm(-2));
    EmitToStreamer(O, MovX0Op0);
    EmitToStreamer(O, MovX1Op1);
    EmitToStreamer(O, MCInstBuilder(AArch64::BL).addExpr(Sym));
    O.AddComment("End XRay custom event");
    EmitToStreamer(O, MCInstBuilder(AArch64::LDPXpost)
                          .addReg(AArch64::SP)
                          .addReg(AArch64::X0)
                          .addReg(AArch64::X1)
                          .addReg(AArch64::SP)
                          .addImm(2));

    recordSled(CurSled, MI, SledKind::CUSTOM_EVENT, 2);
  }
}

void AArch64AsmPrinter::LowerKCFI_CHECK(const MachineInstr &MI) {
  Register AddrReg = MI.getOperand(0).getReg();
  assert(std::next(MI.getIterator())->isCall() &&
         "KCFI_CHECK not followed by a call instruction");
  assert(std::next(MI.getIterator())->getOperand(0).getReg() == AddrReg &&
         "KCFI_CHECK call target doesn't match call operand");

  // Default to using the intra-procedure-call temporary registers for
  // comparing the hashes.
  unsigned ScratchRegs[] = {AArch64::W16, AArch64::W17};
  if (AddrReg == AArch64::XZR) {
    // Checking XZR makes no sense. Instead of emitting a load, zero
    // ScratchRegs[0] and use it for the ESR AddrIndex below.
    AddrReg = getXRegFromWReg(ScratchRegs[0]);
    EmitToStreamer(*OutStreamer, MCInstBuilder(AArch64::ORRXrs)
                                     .addReg(AddrReg)
                                     .addReg(AArch64::XZR)
                                     .addReg(AArch64::XZR)
                                     .addImm(0));
  } else {
    // If one of the scratch registers is used for the call target (e.g.
    // with AArch64::TCRETURNriBTI), we can clobber another caller-saved
    // temporary register instead (in this case, AArch64::W9) as the check
    // is immediately followed by the call instruction.
    for (auto &Reg : ScratchRegs) {
      if (Reg == getWRegFromXReg(AddrReg)) {
        Reg = AArch64::W9;
        break;
      }
    }
    assert(ScratchRegs[0] != AddrReg && ScratchRegs[1] != AddrReg &&
           "Invalid scratch registers for KCFI_CHECK");

    // Adjust the offset for patchable-function-prefix. This assumes that
    // patchable-function-prefix is the same for all functions.
    int64_t PrefixNops = 0;
    (void)MI.getMF()
        ->getFunction()
        .getFnAttribute("patchable-function-prefix")
        .getValueAsString()
        .getAsInteger(10, PrefixNops);

    // Load the target function type hash.
    EmitToStreamer(*OutStreamer, MCInstBuilder(AArch64::LDURWi)
                                     .addReg(ScratchRegs[0])
                                     .addReg(AddrReg)
                                     .addImm(-(PrefixNops * 4 + 4)));
  }

  // Load the expected type hash.
  const int64_t Type = MI.getOperand(1).getImm();
  EmitToStreamer(*OutStreamer, MCInstBuilder(AArch64::MOVKWi)
                                   .addReg(ScratchRegs[1])
                                   .addReg(ScratchRegs[1])
                                   .addImm(Type & 0xFFFF)
                                   .addImm(0));
  EmitToStreamer(*OutStreamer, MCInstBuilder(AArch64::MOVKWi)
                                   .addReg(ScratchRegs[1])
                                   .addReg(ScratchRegs[1])
                                   .addImm((Type >> 16) & 0xFFFF)
                                   .addImm(16));

  // Compare the hashes and trap if there's a mismatch.
  EmitToStreamer(*OutStreamer, MCInstBuilder(AArch64::SUBSWrs)
                                   .addReg(AArch64::WZR)
                                   .addReg(ScratchRegs[0])
                                   .addReg(ScratchRegs[1])
                                   .addImm(0));

  MCSymbol *Pass = OutContext.createTempSymbol();
  EmitToStreamer(*OutStreamer,
                 MCInstBuilder(AArch64::Bcc)
                     .addImm(AArch64CC::EQ)
                     .addExpr(MCSymbolRefExpr::create(Pass, OutContext)));

  // The base ESR is 0x8000 and the register information is encoded in bits
  // 0-9 as follows:
  // - 0-4: n, where the register Xn contains the target address
  // - 5-9: m, where the register Wm contains the expected type hash
  // Where n, m are in [0, 30].
  unsigned TypeIndex = ScratchRegs[1] - AArch64::W0;
  unsigned AddrIndex;
  switch (AddrReg) {
  default:
    AddrIndex = AddrReg - AArch64::X0;
    break;
  case AArch64::FP:
    AddrIndex = 29;
    break;
  case AArch64::LR:
    AddrIndex = 30;
    break;
  }

  assert(AddrIndex < 31 && TypeIndex < 31);

  unsigned ESR = 0x8000 | ((TypeIndex & 31) << 5) | (AddrIndex & 31);
  EmitToStreamer(*OutStreamer, MCInstBuilder(AArch64::BRK).addImm(ESR));
  OutStreamer->emitLabel(Pass);
}

class PtrauthABIVersionDiagnosticInfo : public DiagnosticInfo {
  int V1, V2;
  bool K1, K2;
public:
  PtrauthABIVersionDiagnosticInfo(int V1, bool K1, int V2, bool K2)
      : DiagnosticInfo(DK_Linker, DS_Warning), V1(V1), V2(V2), K1(K1), K2(K2) {}
  void print(DiagnosticPrinter &DP) const override {
    const char *Mode1 = K1 ? "kernel" : "user";
    const char *Mode2 = K2 ? "kernel" : "user";
    DP << "incompatible ptrauth ABI versions: " << V1 << " (" << Mode1
       << ") and " << V2 << " (" << Mode2 << "), falling back to 63 (user)";
  }
};

void AArch64AsmPrinter::EmitPtrAuthVersion(Module &M) {
  // Emit the ptrauth ABI version, if any.
  SmallVector<Module::PtrAuthABIVersion, 2> Versions =
      M.getPtrAuthABIVersions();
  if (Versions.size() == 0)
    return;

  // The ptrauth ABI version is an arm64e concept, only implemented for MachO.
  const Triple &TT = TM.getTargetTriple();
  if (!TT.isOSBinFormatMachO())
    report_fatal_error("ptrauth ABI version support not yet implemented");

  LLVMContext &Ctx = M.getContext();

  Module::PtrAuthABIVersion V = Versions[0];
  if (Versions.size() == 1) {
    if (V.Version > 63) {
      Ctx.emitError("invalid ptrauth ABI version: " + utostr(V.Version));
      V.Version = 63;
      V.Kernel = false;
    }
  }
  // If there are multiple versions, there's a mismatch.  In that case, fall
  // back to version "15", and emit a warning through the context.
  if (Versions.size() == 2) {
    int LV = Versions[0].Version;
    bool LK = Versions[0].Kernel;
    int RV = Versions[1].Version;
    bool RK = Versions[1].Kernel;
    V.Version = 63;
    V.Kernel = false;
    Ctx.diagnose(PtrauthABIVersionDiagnosticInfo(LV, LK, RV, RK));
  }
  assert(Versions.size() <= 2 &&
         "Mismatch between more than two ptrauth abi versions.");

  OutStreamer->EmitPtrAuthABIVersion(V.Version, V.Kernel);
}

void AArch64AsmPrinter::LowerHWASAN_CHECK_MEMACCESS(const MachineInstr &MI) {
  Register Reg = MI.getOperand(0).getReg();
  bool IsShort =
      MI.getOpcode() == AArch64::HWASAN_CHECK_MEMACCESS_SHORTGRANULES;
  uint32_t AccessInfo = MI.getOperand(1).getImm();
  MCSymbol *&Sym =
      HwasanMemaccessSymbols[HwasanMemaccessTuple(Reg, IsShort, AccessInfo)];
  if (!Sym) {
    // FIXME: Make this work on non-ELF.
    if (!TM.getTargetTriple().isOSBinFormatELF())
      report_fatal_error("llvm.hwasan.check.memaccess only supported on ELF");

    std::string SymName = "__hwasan_check_x" + utostr(Reg - AArch64::X0) + "_" +
                          utostr(AccessInfo);
    if (IsShort)
      SymName += "_short_v2";
    Sym = OutContext.getOrCreateSymbol(SymName);
  }

  EmitToStreamer(*OutStreamer,
                 MCInstBuilder(AArch64::BL)
                     .addExpr(MCSymbolRefExpr::create(Sym, OutContext)));
}

void AArch64AsmPrinter::emitHwasanMemaccessSymbols(Module &M) {
  if (HwasanMemaccessSymbols.empty())
    return;

  const Triple &TT = TM.getTargetTriple();
  assert(TT.isOSBinFormatELF());
  std::unique_ptr<MCSubtargetInfo> STI(
      TM.getTarget().createMCSubtargetInfo(TT.str(), "", ""));
  assert(STI && "Unable to create subtarget info");

  MCSymbol *HwasanTagMismatchV1Sym =
      OutContext.getOrCreateSymbol("__hwasan_tag_mismatch");
  MCSymbol *HwasanTagMismatchV2Sym =
      OutContext.getOrCreateSymbol("__hwasan_tag_mismatch_v2");

  const MCSymbolRefExpr *HwasanTagMismatchV1Ref =
      MCSymbolRefExpr::create(HwasanTagMismatchV1Sym, OutContext);
  const MCSymbolRefExpr *HwasanTagMismatchV2Ref =
      MCSymbolRefExpr::create(HwasanTagMismatchV2Sym, OutContext);

  for (auto &P : HwasanMemaccessSymbols) {
    unsigned Reg = std::get<0>(P.first);
    bool IsShort = std::get<1>(P.first);
    uint32_t AccessInfo = std::get<2>(P.first);
    const MCSymbolRefExpr *HwasanTagMismatchRef =
        IsShort ? HwasanTagMismatchV2Ref : HwasanTagMismatchV1Ref;
    MCSymbol *Sym = P.second;

    bool HasMatchAllTag =
        (AccessInfo >> HWASanAccessInfo::HasMatchAllShift) & 1;
    uint8_t MatchAllTag =
        (AccessInfo >> HWASanAccessInfo::MatchAllShift) & 0xff;
    unsigned Size =
        1 << ((AccessInfo >> HWASanAccessInfo::AccessSizeShift) & 0xf);
    bool CompileKernel =
        (AccessInfo >> HWASanAccessInfo::CompileKernelShift) & 1;

    OutStreamer->switchSection(OutContext.getELFSection(
        ".text.hot", ELF::SHT_PROGBITS,
        ELF::SHF_EXECINSTR | ELF::SHF_ALLOC | ELF::SHF_GROUP, 0, Sym->getName(),
        /*IsComdat=*/true));

    OutStreamer->emitSymbolAttribute(Sym, MCSA_ELF_TypeFunction);
    OutStreamer->emitSymbolAttribute(Sym, MCSA_Weak);
    OutStreamer->emitSymbolAttribute(Sym, MCSA_Hidden);
    OutStreamer->emitLabel(Sym);

    OutStreamer->emitInstruction(MCInstBuilder(AArch64::SBFMXri)
                                     .addReg(AArch64::X16)
                                     .addReg(Reg)
                                     .addImm(4)
                                     .addImm(55),
                                 *STI);
    OutStreamer->emitInstruction(
        MCInstBuilder(AArch64::LDRBBroX)
            .addReg(AArch64::W16)
            .addReg(IsShort ? AArch64::X20 : AArch64::X9)
            .addReg(AArch64::X16)
            .addImm(0)
            .addImm(0),
        *STI);
    OutStreamer->emitInstruction(
        MCInstBuilder(AArch64::SUBSXrs)
            .addReg(AArch64::XZR)
            .addReg(AArch64::X16)
            .addReg(Reg)
            .addImm(AArch64_AM::getShifterImm(AArch64_AM::LSR, 56)),
        *STI);
    MCSymbol *HandleMismatchOrPartialSym = OutContext.createTempSymbol();
    OutStreamer->emitInstruction(
        MCInstBuilder(AArch64::Bcc)
            .addImm(AArch64CC::NE)
            .addExpr(MCSymbolRefExpr::create(HandleMismatchOrPartialSym,
                                             OutContext)),
        *STI);
    MCSymbol *ReturnSym = OutContext.createTempSymbol();
    OutStreamer->emitLabel(ReturnSym);
    OutStreamer->emitInstruction(
        MCInstBuilder(AArch64::RET).addReg(AArch64::LR), *STI);
    OutStreamer->emitLabel(HandleMismatchOrPartialSym);

    if (HasMatchAllTag) {
      OutStreamer->emitInstruction(MCInstBuilder(AArch64::UBFMXri)
                                       .addReg(AArch64::X17)
                                       .addReg(Reg)
                                       .addImm(56)
                                       .addImm(63),
                                   *STI);
      OutStreamer->emitInstruction(MCInstBuilder(AArch64::SUBSXri)
                                       .addReg(AArch64::XZR)
                                       .addReg(AArch64::X17)
                                       .addImm(MatchAllTag)
                                       .addImm(0),
                                   *STI);
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::Bcc)
              .addImm(AArch64CC::EQ)
              .addExpr(MCSymbolRefExpr::create(ReturnSym, OutContext)),
          *STI);
    }

    if (IsShort) {
      OutStreamer->emitInstruction(MCInstBuilder(AArch64::SUBSWri)
                                       .addReg(AArch64::WZR)
                                       .addReg(AArch64::W16)
                                       .addImm(15)
                                       .addImm(0),
                                   *STI);
      MCSymbol *HandleMismatchSym = OutContext.createTempSymbol();
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::Bcc)
              .addImm(AArch64CC::HI)
              .addExpr(MCSymbolRefExpr::create(HandleMismatchSym, OutContext)),
          *STI);

      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::ANDXri)
              .addReg(AArch64::X17)
              .addReg(Reg)
              .addImm(AArch64_AM::encodeLogicalImmediate(0xf, 64)),
          *STI);
      if (Size != 1)
        OutStreamer->emitInstruction(MCInstBuilder(AArch64::ADDXri)
                                         .addReg(AArch64::X17)
                                         .addReg(AArch64::X17)
                                         .addImm(Size - 1)
                                         .addImm(0),
                                     *STI);
      OutStreamer->emitInstruction(MCInstBuilder(AArch64::SUBSWrs)
                                       .addReg(AArch64::WZR)
                                       .addReg(AArch64::W16)
                                       .addReg(AArch64::W17)
                                       .addImm(0),
                                   *STI);
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::Bcc)
              .addImm(AArch64CC::LS)
              .addExpr(MCSymbolRefExpr::create(HandleMismatchSym, OutContext)),
          *STI);

      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::ORRXri)
              .addReg(AArch64::X16)
              .addReg(Reg)
              .addImm(AArch64_AM::encodeLogicalImmediate(0xf, 64)),
          *STI);
      OutStreamer->emitInstruction(MCInstBuilder(AArch64::LDRBBui)
                                       .addReg(AArch64::W16)
                                       .addReg(AArch64::X16)
                                       .addImm(0),
                                   *STI);
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::SUBSXrs)
              .addReg(AArch64::XZR)
              .addReg(AArch64::X16)
              .addReg(Reg)
              .addImm(AArch64_AM::getShifterImm(AArch64_AM::LSR, 56)),
          *STI);
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::Bcc)
              .addImm(AArch64CC::EQ)
              .addExpr(MCSymbolRefExpr::create(ReturnSym, OutContext)),
          *STI);

      OutStreamer->emitLabel(HandleMismatchSym);
    }

    OutStreamer->emitInstruction(MCInstBuilder(AArch64::STPXpre)
                                     .addReg(AArch64::SP)
                                     .addReg(AArch64::X0)
                                     .addReg(AArch64::X1)
                                     .addReg(AArch64::SP)
                                     .addImm(-32),
                                 *STI);
    OutStreamer->emitInstruction(MCInstBuilder(AArch64::STPXi)
                                     .addReg(AArch64::FP)
                                     .addReg(AArch64::LR)
                                     .addReg(AArch64::SP)
                                     .addImm(29),
                                 *STI);

    if (Reg != AArch64::X0)
      OutStreamer->emitInstruction(MCInstBuilder(AArch64::ORRXrs)
                                       .addReg(AArch64::X0)
                                       .addReg(AArch64::XZR)
                                       .addReg(Reg)
                                       .addImm(0),
                                   *STI);
    OutStreamer->emitInstruction(
        MCInstBuilder(AArch64::MOVZXi)
            .addReg(AArch64::X1)
            .addImm(AccessInfo & HWASanAccessInfo::RuntimeMask)
            .addImm(0),
        *STI);

    if (CompileKernel) {
      // The Linux kernel's dynamic loader doesn't support GOT relative
      // relocations, but it doesn't support late binding either, so just call
      // the function directly.
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::B).addExpr(HwasanTagMismatchRef), *STI);
    } else {
      // Intentionally load the GOT entry and branch to it, rather than possibly
      // late binding the function, which may clobber the registers before we
      // have a chance to save them.
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::ADRP)
              .addReg(AArch64::X16)
              .addExpr(AArch64MCExpr::create(
                  HwasanTagMismatchRef, AArch64MCExpr::VariantKind::VK_GOT_PAGE,
                  OutContext)),
          *STI);
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::LDRXui)
              .addReg(AArch64::X16)
              .addReg(AArch64::X16)
              .addExpr(AArch64MCExpr::create(
                  HwasanTagMismatchRef, AArch64MCExpr::VariantKind::VK_GOT_LO12,
                  OutContext)),
          *STI);
      OutStreamer->emitInstruction(
          MCInstBuilder(AArch64::BR).addReg(AArch64::X16), *STI);
    }
  }
}

static void
emitAuthenticatedPointer(MCStreamer &OutStreamer, MCSymbol *StubLabel,
                         const MachineModuleInfoMachO::AuthStubInfo &StubInfo) {
  // L_foo$addend$auth_ptr$ib$23:
  OutStreamer.emitLabel(StubLabel);
  OutStreamer.emitValue(StubInfo.Pointer, /*size=*/8);
}

void AArch64AsmPrinter::emitEndOfAsmFile(Module &M) {
  emitHwasanMemaccessSymbols(M);

  const Triple &TT = TM.getTargetTriple();
  if (TT.isOSBinFormatMachO()) {

    // Output authenticated pointers as indirect symbols, if we have any.
    MachineModuleInfoMachO &MMIMacho =
        MMI->getObjFileInfo<MachineModuleInfoMachO>();

    auto Stubs = MMIMacho.getAuthGVStubList();

    if (!Stubs.empty()) {
      // Switch to the "__auth_ptr" section.
      OutStreamer->switchSection(
          OutContext.getMachOSection("__DATA", "__auth_ptr", MachO::S_REGULAR,
                                     SectionKind::getMetadata()));
      emitAlignment(Align(8));

      for (auto &Stub : Stubs)
        emitAuthenticatedPointer(*OutStreamer, Stub.first, Stub.second);

      OutStreamer->addBlankLine();
    }

    // Funny Darwin hack: This flag tells the linker that no global symbols
    // contain code that falls through to other global symbols (e.g. the obvious
    // implementation of multiple entry points).  If this doesn't occur, the
    // linker can safely perform dead code stripping.  Since LLVM never
    // generates code that does this, it is always safe to set.
    OutStreamer->emitAssemblerFlag(MCAF_SubsectionsViaSymbols);
  }

  // Emit stack and fault map information.
  FM.serializeToFaultMapSection();

}

void AArch64AsmPrinter::emitLOHs() {
  SmallVector<MCSymbol *, 3> MCArgs;

  for (const auto &D : AArch64FI->getLOHContainer()) {
    for (const MachineInstr *MI : D.getArgs()) {
      MInstToMCSymbol::iterator LabelIt = LOHInstToLabel.find(MI);
      assert(LabelIt != LOHInstToLabel.end() &&
             "Label hasn't been inserted for LOH related instruction");
      MCArgs.push_back(LabelIt->second);
    }
    OutStreamer->emitLOHDirective(D.getKind(), MCArgs);
    MCArgs.clear();
  }
}

void AArch64AsmPrinter::emitFunctionBodyEnd() {
  if (!AArch64FI->getLOHRelated().empty())
    emitLOHs();
}

/// GetCPISymbol - Return the symbol for the specified constant pool entry.
MCSymbol *AArch64AsmPrinter::GetCPISymbol(unsigned CPID) const {
  // Darwin uses a linker-private symbol name for constant-pools (to
  // avoid addends on the relocation?), ELF has no such concept and
  // uses a normal private symbol.
  if (!getDataLayout().getLinkerPrivateGlobalPrefix().empty())
    return OutContext.getOrCreateSymbol(
        Twine(getDataLayout().getLinkerPrivateGlobalPrefix()) + "CPI" +
        Twine(getFunctionNumber()) + "_" + Twine(CPID));

  return AsmPrinter::GetCPISymbol(CPID);
}

void AArch64AsmPrinter::printOperand(const MachineInstr *MI, unsigned OpNum,
                                     raw_ostream &O) {
  const MachineOperand &MO = MI->getOperand(OpNum);
  switch (MO.getType()) {
  default:
    llvm_unreachable("<unknown operand type>");
  case MachineOperand::MO_Register: {
    Register Reg = MO.getReg();
    assert(Reg.isPhysical());
    assert(!MO.getSubReg() && "Subregs should be eliminated!");
    O << AArch64InstPrinter::getRegisterName(Reg);
    break;
  }
  case MachineOperand::MO_Immediate: {
    O << MO.getImm();
    break;
  }
  case MachineOperand::MO_GlobalAddress: {
    PrintSymbolOperand(MO, O);
    break;
  }
  case MachineOperand::MO_BlockAddress: {
    MCSymbol *Sym = GetBlockAddressSymbol(MO.getBlockAddress());
    Sym->print(O, MAI);
    break;
  }
  }
}

bool AArch64AsmPrinter::printAsmMRegister(const MachineOperand &MO, char Mode,
                                          raw_ostream &O) {
  Register Reg = MO.getReg();
  switch (Mode) {
  default:
    return true; // Unknown mode.
  case 'w':
    Reg = getWRegFromXReg(Reg);
    break;
  case 'x':
    Reg = getXRegFromWReg(Reg);
    break;
  case 't':
    Reg = getXRegFromXRegTuple(Reg);
    break;
  }

  O << AArch64InstPrinter::getRegisterName(Reg);
  return false;
}

// Prints the register in MO using class RC using the offset in the
// new register class. This should not be used for cross class
// printing.
bool AArch64AsmPrinter::printAsmRegInClass(const MachineOperand &MO,
                                           const TargetRegisterClass *RC,
                                           unsigned AltName, raw_ostream &O) {
  assert(MO.isReg() && "Should only get here with a register!");
  const TargetRegisterInfo *RI = STI->getRegisterInfo();
  Register Reg = MO.getReg();
  unsigned RegToPrint = RC->getRegister(RI->getEncodingValue(Reg));
  if (!RI->regsOverlap(RegToPrint, Reg))
    return true;
  O << AArch64InstPrinter::getRegisterName(RegToPrint, AltName);
  return false;
}

bool AArch64AsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNum,
                                        const char *ExtraCode, raw_ostream &O) {
  const MachineOperand &MO = MI->getOperand(OpNum);

  // First try the generic code, which knows about modifiers like 'c' and 'n'.
  if (!AsmPrinter::PrintAsmOperand(MI, OpNum, ExtraCode, O))
    return false;

  // Does this asm operand have a single letter operand modifier?
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0)
      return true; // Unknown modifier.

    switch (ExtraCode[0]) {
    default:
      return true; // Unknown modifier.
    case 'w':      // Print W register
    case 'x':      // Print X register
      if (MO.isReg())
        return printAsmMRegister(MO, ExtraCode[0], O);
      if (MO.isImm() && MO.getImm() == 0) {
        unsigned Reg = ExtraCode[0] == 'w' ? AArch64::WZR : AArch64::XZR;
        O << AArch64InstPrinter::getRegisterName(Reg);
        return false;
      }
      printOperand(MI, OpNum, O);
      return false;
    case 'b': // Print B register.
    case 'h': // Print H register.
    case 's': // Print S register.
    case 'd': // Print D register.
    case 'q': // Print Q register.
    case 'z': // Print Z register.
      if (MO.isReg()) {
        const TargetRegisterClass *RC;
        switch (ExtraCode[0]) {
        case 'b':
          RC = &AArch64::FPR8RegClass;
          break;
        case 'h':
          RC = &AArch64::FPR16RegClass;
          break;
        case 's':
          RC = &AArch64::FPR32RegClass;
          break;
        case 'd':
          RC = &AArch64::FPR64RegClass;
          break;
        case 'q':
          RC = &AArch64::FPR128RegClass;
          break;
        case 'z':
          RC = &AArch64::ZPRRegClass;
          break;
        default:
          return true;
        }
        return printAsmRegInClass(MO, RC, AArch64::NoRegAltName, O);
      }
      printOperand(MI, OpNum, O);
      return false;
    }
  }

  // According to ARM, we should emit x and v registers unless we have a
  // modifier.
  if (MO.isReg()) {
    Register Reg = MO.getReg();

    // If this is a w or x register, print an x register.
    if (AArch64::GPR32allRegClass.contains(Reg) ||
        AArch64::GPR64allRegClass.contains(Reg))
      return printAsmMRegister(MO, 'x', O);

    // If this is an x register tuple, print an x register.
    if (AArch64::GPR64x8ClassRegClass.contains(Reg))
      return printAsmMRegister(MO, 't', O);

    unsigned AltName = AArch64::NoRegAltName;
    const TargetRegisterClass *RegClass;
    if (AArch64::ZPRRegClass.contains(Reg)) {
      RegClass = &AArch64::ZPRRegClass;
    } else if (AArch64::PPRRegClass.contains(Reg)) {
      RegClass = &AArch64::PPRRegClass;
    } else if (AArch64::PNRRegClass.contains(Reg)) {
      RegClass = &AArch64::PNRRegClass;
    } else {
      RegClass = &AArch64::FPR128RegClass;
      AltName = AArch64::vreg;
    }

    // If this is a b, h, s, d, or q register, print it as a v register.
    return printAsmRegInClass(MO, RegClass, AltName, O);
  }

  printOperand(MI, OpNum, O);
  return false;
}

bool AArch64AsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                              unsigned OpNum,
                                              const char *ExtraCode,
                                              raw_ostream &O) {
  if (ExtraCode && ExtraCode[0] && ExtraCode[0] != 'a')
    return true; // Unknown modifier.

  const MachineOperand &MO = MI->getOperand(OpNum);
  assert(MO.isReg() && "unexpected inline asm memory operand");
  O << "[" << AArch64InstPrinter::getRegisterName(MO.getReg()) << "]";
  return false;
}

void AArch64AsmPrinter::PrintDebugValueComment(const MachineInstr *MI,
                                               raw_ostream &OS) {
  unsigned NOps = MI->getNumOperands();
  assert(NOps == 4);
  OS << '\t' << MAI->getCommentString() << "DEBUG_VALUE: ";
  // cast away const; DIetc do not take const operands for some reason.
  OS << MI->getDebugVariable()->getName();
  OS << " <- ";
  // Frame address.  Currently handles register +- offset only.
  assert(MI->isIndirectDebugValue());
  OS << '[';
  for (unsigned I = 0, E = std::distance(MI->debug_operands().begin(),
                                         MI->debug_operands().end());
       I < E; ++I) {
    if (I != 0)
      OS << ", ";
    printOperand(MI, I, OS);
  }
  OS << ']';
  OS << "+";
  printOperand(MI, NOps - 2, OS);
}

void AArch64AsmPrinter::emitJumpTableInfo() {
  const MachineJumpTableInfo *MJTI = MF->getJumpTableInfo();
  if (!MJTI) return;

  const std::vector<MachineJumpTableEntry> &JT = MJTI->getJumpTables();
  if (JT.empty()) return;

  const TargetLoweringObjectFile &TLOF = getObjFileLowering();
  MCSection *ReadOnlySec = TLOF.getSectionForJumpTable(MF->getFunction(), TM);
  OutStreamer->switchSection(ReadOnlySec);

  auto AFI = MF->getInfo<AArch64FunctionInfo>();
  for (unsigned JTI = 0, e = JT.size(); JTI != e; ++JTI) {
    const std::vector<MachineBasicBlock*> &JTBBs = JT[JTI].MBBs;

    // If this jump table was deleted, ignore it.
    if (JTBBs.empty()) continue;

    unsigned Size = AFI->getJumpTableEntrySize(JTI);
    emitAlignment(Align(Size));
    OutStreamer->emitLabel(GetJTISymbol(JTI));

    const MCSymbol *BaseSym = AArch64FI->getJumpTableEntryPCRelSymbol(JTI);
    const MCExpr *Base = MCSymbolRefExpr::create(BaseSym, OutContext);

    for (auto *JTBB : JTBBs) {
      const MCExpr *Value =
          MCSymbolRefExpr::create(JTBB->getSymbol(), OutContext);

      // Each entry is:
      //     .byte/.hword (LBB - Lbase)>>2
      // or plain:
      //     .word LBB - Lbase
      Value = MCBinaryExpr::createSub(Value, Base, OutContext);
      if (Size != 4)
        Value = MCBinaryExpr::createLShr(
            Value, MCConstantExpr::create(2, OutContext), OutContext);

      OutStreamer->emitValue(Value, Size);
    }
  }
}

std::tuple<const MCSymbol *, uint64_t, const MCSymbol *,
           codeview::JumpTableEntrySize>
AArch64AsmPrinter::getCodeViewJumpTableInfo(int JTI,
                                            const MachineInstr *BranchInstr,
                                            const MCSymbol *BranchLabel) const {
  const auto AFI = MF->getInfo<AArch64FunctionInfo>();
  const auto Base = AArch64FI->getJumpTableEntryPCRelSymbol(JTI);
  codeview::JumpTableEntrySize EntrySize;
  switch (AFI->getJumpTableEntrySize(JTI)) {
  case 1:
    EntrySize = codeview::JumpTableEntrySize::UInt8ShiftLeft;
    break;
  case 2:
    EntrySize = codeview::JumpTableEntrySize::UInt16ShiftLeft;
    break;
  case 4:
    EntrySize = codeview::JumpTableEntrySize::Int32;
    break;
  default:
    llvm_unreachable("Unexpected jump table entry size");
  }
  return std::make_tuple(Base, 0, BranchLabel, EntrySize);
}

void AArch64AsmPrinter::emitFunctionEntryLabel() {
  if (MF->getFunction().getCallingConv() == CallingConv::AArch64_VectorCall ||
      MF->getFunction().getCallingConv() ==
          CallingConv::AArch64_SVE_VectorCall ||
      MF->getInfo<AArch64FunctionInfo>()->isSVECC()) {
    auto *TS =
        static_cast<AArch64TargetStreamer *>(OutStreamer->getTargetStreamer());
    TS->emitDirectiveVariantPCS(CurrentFnSym);
  }

  return AsmPrinter::emitFunctionEntryLabel();
}

/// Small jump tables contain an unsigned byte or half, representing the offset
/// from the lowest-addressed possible destination to the desired basic
/// block. Since all instructions are 4-byte aligned, this is further compressed
/// by counting in instructions rather than bytes (i.e. divided by 4). So, to
/// materialize the correct destination we need:
///
///             adr xDest, .LBB0_0
///             ldrb wScratch, [xTable, xEntry]   (with "lsl #1" for ldrh).
///             add xDest, xDest, xScratch (with "lsl #2" for smaller entries)
void AArch64AsmPrinter::LowerJumpTableDest(llvm::MCStreamer &OutStreamer,
                                           const llvm::MachineInstr &MI) {
  Register DestReg = MI.getOperand(0).getReg();
  Register ScratchReg = MI.getOperand(1).getReg();
  Register ScratchRegW =
      STI->getRegisterInfo()->getSubReg(ScratchReg, AArch64::sub_32);
  Register TableReg = MI.getOperand(2).getReg();
  Register EntryReg = MI.getOperand(3).getReg();
  int JTIdx = MI.getOperand(4).getIndex();
  int Size = AArch64FI->getJumpTableEntrySize(JTIdx);

  // This has to be first because the compression pass based its reachability
  // calculations on the start of the JumpTableDest instruction.
  auto Label =
      MF->getInfo<AArch64FunctionInfo>()->getJumpTableEntryPCRelSymbol(JTIdx);

  // If we don't already have a symbol to use as the base, use the ADR
  // instruction itself.
  if (!Label) {
    Label = MF->getContext().createTempSymbol();
    AArch64FI->setJumpTableEntryInfo(JTIdx, Size, Label);
    OutStreamer.emitLabel(Label);
  }

  auto LabelExpr = MCSymbolRefExpr::create(Label, MF->getContext());
  EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::ADR)
                                  .addReg(DestReg)
                                  .addExpr(LabelExpr));

  // Load the number of instruction-steps to offset from the label.
  unsigned LdrOpcode;
  switch (Size) {
  case 1: LdrOpcode = AArch64::LDRBBroX; break;
  case 2: LdrOpcode = AArch64::LDRHHroX; break;
  case 4: LdrOpcode = AArch64::LDRSWroX; break;
  default:
    llvm_unreachable("Unknown jump table size");
  }

  EmitToStreamer(OutStreamer, MCInstBuilder(LdrOpcode)
                                  .addReg(Size == 4 ? ScratchReg : ScratchRegW)
                                  .addReg(TableReg)
                                  .addReg(EntryReg)
                                  .addImm(0)
                                  .addImm(Size == 1 ? 0 : 1));

  // Add to the already materialized base label address, multiplying by 4 if
  // compressed.
  EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::ADDXrs)
                                  .addReg(DestReg)
                                  .addReg(DestReg)
                                  .addReg(ScratchReg)
                                  .addImm(Size == 4 ? 0 : 2));
}

void AArch64AsmPrinter::LowerMOPS(llvm::MCStreamer &OutStreamer,
                                  const llvm::MachineInstr &MI) {
  unsigned Opcode = MI.getOpcode();
  assert(STI->hasMOPS());
  assert(STI->hasMTE() || Opcode != AArch64::MOPSMemorySetTaggingPseudo);

  const auto Ops = [Opcode]() -> std::array<unsigned, 3> {
    if (Opcode == AArch64::MOPSMemoryCopyPseudo)
      return {AArch64::CPYFP, AArch64::CPYFM, AArch64::CPYFE};
    if (Opcode == AArch64::MOPSMemoryMovePseudo)
      return {AArch64::CPYP, AArch64::CPYM, AArch64::CPYE};
    if (Opcode == AArch64::MOPSMemorySetPseudo)
      return {AArch64::SETP, AArch64::SETM, AArch64::SETE};
    if (Opcode == AArch64::MOPSMemorySetTaggingPseudo)
      return {AArch64::SETGP, AArch64::SETGM, AArch64::MOPSSETGE};
    llvm_unreachable("Unhandled memory operation pseudo");
  }();
  const bool IsSet = Opcode == AArch64::MOPSMemorySetPseudo ||
                     Opcode == AArch64::MOPSMemorySetTaggingPseudo;

  for (auto Op : Ops) {
    int i = 0;
    auto MCIB = MCInstBuilder(Op);
    // Destination registers
    MCIB.addReg(MI.getOperand(i++).getReg());
    MCIB.addReg(MI.getOperand(i++).getReg());
    if (!IsSet)
      MCIB.addReg(MI.getOperand(i++).getReg());
    // Input registers
    MCIB.addReg(MI.getOperand(i++).getReg());
    MCIB.addReg(MI.getOperand(i++).getReg());
    MCIB.addReg(MI.getOperand(i++).getReg());

    EmitToStreamer(OutStreamer, MCIB);
  }
}

void AArch64AsmPrinter::LowerSTACKMAP(MCStreamer &OutStreamer, StackMaps &SM,
                                      const MachineInstr &MI) {
  unsigned NumNOPBytes = StackMapOpers(&MI).getNumPatchBytes();

  auto &Ctx = OutStreamer.getContext();
  MCSymbol *MILabel = Ctx.createTempSymbol();
  OutStreamer.emitLabel(MILabel);

  SM.recordStackMap(*MILabel, MI);
  assert(NumNOPBytes % 4 == 0 && "Invalid number of NOP bytes requested!");

  // Scan ahead to trim the shadow.
  const MachineBasicBlock &MBB = *MI.getParent();
  MachineBasicBlock::const_iterator MII(MI);
  ++MII;
  while (NumNOPBytes > 0) {
    if (MII == MBB.end() || MII->isCall() ||
        MII->getOpcode() == AArch64::DBG_VALUE ||
        MII->getOpcode() == TargetOpcode::PATCHPOINT ||
        MII->getOpcode() == TargetOpcode::STACKMAP)
      break;
    ++MII;
    NumNOPBytes -= 4;
  }

  // Emit nops.
  for (unsigned i = 0; i < NumNOPBytes; i += 4)
    EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::HINT).addImm(0));
}

// Lower a patchpoint of the form:
// [<def>], <id>, <numBytes>, <target>, <numArgs>
void AArch64AsmPrinter::LowerPATCHPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                                        const MachineInstr &MI) {
  auto &Ctx = OutStreamer.getContext();
  MCSymbol *MILabel = Ctx.createTempSymbol();
  OutStreamer.emitLabel(MILabel);
  SM.recordPatchPoint(*MILabel, MI);

  PatchPointOpers Opers(&MI);

  int64_t CallTarget = Opers.getCallTarget().getImm();
  unsigned EncodedBytes = 0;
  if (CallTarget) {
    assert((CallTarget & 0xFFFFFFFFFFFF) == CallTarget &&
           "High 16 bits of call target should be zero.");
    Register ScratchReg = MI.getOperand(Opers.getNextScratchIdx()).getReg();
    EncodedBytes = 16;
    // Materialize the jump address:
    EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::MOVZXi)
                                    .addReg(ScratchReg)
                                    .addImm((CallTarget >> 32) & 0xFFFF)
                                    .addImm(32));
    EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::MOVKXi)
                                    .addReg(ScratchReg)
                                    .addReg(ScratchReg)
                                    .addImm((CallTarget >> 16) & 0xFFFF)
                                    .addImm(16));
    EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::MOVKXi)
                                    .addReg(ScratchReg)
                                    .addReg(ScratchReg)
                                    .addImm(CallTarget & 0xFFFF)
                                    .addImm(0));
    EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::BLR).addReg(ScratchReg));
  }
  // Emit padding.
  unsigned NumBytes = Opers.getNumPatchBytes();
  assert(NumBytes >= EncodedBytes &&
         "Patchpoint can't request size less than the length of a call.");
  assert((NumBytes - EncodedBytes) % 4 == 0 &&
         "Invalid number of NOP bytes requested!");
  for (unsigned i = EncodedBytes; i < NumBytes; i += 4)
    EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::HINT).addImm(0));
}

void AArch64AsmPrinter::LowerSTATEPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                                        const MachineInstr &MI) {
  StatepointOpers SOpers(&MI);
  if (unsigned PatchBytes = SOpers.getNumPatchBytes()) {
    assert(PatchBytes % 4 == 0 && "Invalid number of NOP bytes requested!");
    for (unsigned i = 0; i < PatchBytes; i += 4)
      EmitToStreamer(OutStreamer, MCInstBuilder(AArch64::HINT).addImm(0));
  } else {
    // Lower call target and choose correct opcode
    const MachineOperand &CallTarget = SOpers.getCallTarget();
    MCOperand CallTargetMCOp;
    unsigned CallOpcode;
    switch (CallTarget.getType()) {
    case MachineOperand::MO_GlobalAddress:
    case MachineOperand::MO_ExternalSymbol:
      MCInstLowering.lowerOperand(CallTarget, CallTargetMCOp);
      CallOpcode = AArch64::BL;
      break;
    case MachineOperand::MO_Immediate:
      CallTargetMCOp = MCOperand::createImm(CallTarget.getImm());
      CallOpcode = AArch64::BL;
      break;
    case MachineOperand::MO_Register:
      CallTargetMCOp = MCOperand::createReg(CallTarget.getReg());
      CallOpcode = AArch64::BLR;
      break;
    default:
      llvm_unreachable("Unsupported operand type in statepoint call target");
      break;
    }

    EmitToStreamer(OutStreamer,
                   MCInstBuilder(CallOpcode).addOperand(CallTargetMCOp));
  }

  auto &Ctx = OutStreamer.getContext();
  MCSymbol *MILabel = Ctx.createTempSymbol();
  OutStreamer.emitLabel(MILabel);
  SM.recordStatepoint(*MILabel, MI);
}

void AArch64AsmPrinter::LowerFAULTING_OP(const MachineInstr &FaultingMI) {
  // FAULTING_LOAD_OP <def>, <faltinf type>, <MBB handler>,
  //                  <opcode>, <operands>

  Register DefRegister = FaultingMI.getOperand(0).getReg();
  FaultMaps::FaultKind FK =
      static_cast<FaultMaps::FaultKind>(FaultingMI.getOperand(1).getImm());
  MCSymbol *HandlerLabel = FaultingMI.getOperand(2).getMBB()->getSymbol();
  unsigned Opcode = FaultingMI.getOperand(3).getImm();
  unsigned OperandsBeginIdx = 4;

  auto &Ctx = OutStreamer->getContext();
  MCSymbol *FaultingLabel = Ctx.createTempSymbol();
  OutStreamer->emitLabel(FaultingLabel);

  assert(FK < FaultMaps::FaultKindMax && "Invalid Faulting Kind!");
  FM.recordFaultingOp(FK, FaultingLabel, HandlerLabel);

  MCInst MI;
  MI.setOpcode(Opcode);

  if (DefRegister != (Register)0)
    MI.addOperand(MCOperand::createReg(DefRegister));

  for (const MachineOperand &MO :
       llvm::drop_begin(FaultingMI.operands(), OperandsBeginIdx)) {
    MCOperand Dest;
    lowerOperand(MO, Dest);
    MI.addOperand(Dest);
  }

  OutStreamer->AddComment("on-fault: " + HandlerLabel->getName());
  OutStreamer->emitInstruction(MI, getSubtargetInfo());
}

void AArch64AsmPrinter::emitFMov0(const MachineInstr &MI) {
  Register DestReg = MI.getOperand(0).getReg();
  if (STI->hasZeroCycleZeroingFP() && !STI->hasZeroCycleZeroingFPWorkaround() &&
      STI->isNeonAvailable()) {
    // Convert H/S register to corresponding D register
    if (AArch64::H0 <= DestReg && DestReg <= AArch64::H31)
      DestReg = AArch64::D0 + (DestReg - AArch64::H0);
    else if (AArch64::S0 <= DestReg && DestReg <= AArch64::S31)
      DestReg = AArch64::D0 + (DestReg - AArch64::S0);
    else
      assert(AArch64::D0 <= DestReg && DestReg <= AArch64::D31);

    MCInst MOVI;
    MOVI.setOpcode(AArch64::MOVID);
    MOVI.addOperand(MCOperand::createReg(DestReg));
    MOVI.addOperand(MCOperand::createImm(0));
    EmitToStreamer(*OutStreamer, MOVI);
  } else {
    MCInst FMov;
    switch (MI.getOpcode()) {
    default: llvm_unreachable("Unexpected opcode");
    case AArch64::FMOVH0:
      FMov.setOpcode(STI->hasFullFP16() ? AArch64::FMOVWHr : AArch64::FMOVWSr);
      if (!STI->hasFullFP16())
        DestReg = (AArch64::S0 + (DestReg - AArch64::H0));
      FMov.addOperand(MCOperand::createReg(DestReg));
      FMov.addOperand(MCOperand::createReg(AArch64::WZR));
      break;
    case AArch64::FMOVS0:
      FMov.setOpcode(AArch64::FMOVWSr);
      FMov.addOperand(MCOperand::createReg(DestReg));
      FMov.addOperand(MCOperand::createReg(AArch64::WZR));
      break;
    case AArch64::FMOVD0:
      FMov.setOpcode(AArch64::FMOVXDr);
      FMov.addOperand(MCOperand::createReg(DestReg));
      FMov.addOperand(MCOperand::createReg(AArch64::XZR));
      break;
    }
    EmitToStreamer(*OutStreamer, FMov);
  }
}


const MCExpr *
AArch64AsmPrinter::lowerPtrAuthGlobalConstant(const GlobalPtrAuthInfo &PAI) {
  MCContext &Ctx = OutContext;

  // Figure out the base symbol and the addend, if any.
  APInt Offset(64, 0);
  const Value *BaseGV =
    PAI.getPointer()->stripAndAccumulateConstantOffsets(
      getDataLayout(), Offset, /*AllowNonInbounds=*/true);

  auto *BaseGVB = dyn_cast<GlobalValue>(BaseGV);

  // If we can't understand the referenced ConstantExpr, there's nothing
  // else we can do: emit an error.
  if (!BaseGVB) {
    BaseGVB = PAI.getGV();

    std::string Buf;
    raw_string_ostream OS(Buf);
    OS << "Couldn't resolve target base/addend of llvm.ptrauth global '"
      << *BaseGVB << "'";
    BaseGV->getContext().emitError(OS.str());
  }

  // If there is an addend, turn that into the appropriate MCExpr.
  const MCExpr *Sym = MCSymbolRefExpr::create(getSymbol(BaseGVB), Ctx);
  if (Offset.sgt(0))
    Sym = MCBinaryExpr::createAdd(
        Sym, MCConstantExpr::create(Offset.getSExtValue(), Ctx), Ctx);
  else if (Offset.slt(0))
    Sym = MCBinaryExpr::createSub(
        Sym, MCConstantExpr::create((-Offset).getSExtValue(), Ctx), Ctx);

  auto *Disc = PAI.getDiscriminator();
  uint64_t KeyID = PAI.getKey()->getZExtValue();
  if (!isUInt<2>(KeyID))
    BaseGV->getContext().emitError(
        "Invalid AArch64 PAC Key ID '" + utostr(KeyID) + "' in llvm.ptrauth global '" +
        BaseGV->getName() + "'");

  // Finally build the complete @AUTH expr.
  return AArch64AuthMCExpr::create(Sym, Disc->getZExtValue(),
                                   AArch64PACKey::ID(KeyID),
                                   PAI.hasAddressDiversity(), Ctx);
}

// Simple pseudo-instructions have their lowering (with expansion to real
// instructions) auto-generated.
#include "AArch64GenMCPseudoLowering.inc"

void AArch64AsmPrinter::emitInstruction(const MachineInstr *MI) {
  AArch64_MC::verifyInstructionPredicates(MI->getOpcode(), STI->getFeatureBits());

  // Do any auto-generated pseudo lowerings.
  if (emitPseudoExpansionLowering(*OutStreamer, MI))
    return;

  if (MI->getOpcode() == AArch64::ADRP) {
    for (auto &Opd : MI->operands()) {
      if (Opd.isSymbol() && StringRef(Opd.getSymbolName()) ==
                                "swift_async_extendedFramePointerFlags") {
        ShouldEmitWeakSwiftAsyncExtendedFramePointerFlags = true;
      }
    }
  }

  if (AArch64FI->getLOHRelated().count(MI)) {
    // Generate a label for LOH related instruction
    MCSymbol *LOHLabel = createTempSymbol("loh");
    // Associate the instruction with the label
    LOHInstToLabel[MI] = LOHLabel;
    OutStreamer->emitLabel(LOHLabel);
  }

  AArch64TargetStreamer *TS =
    static_cast<AArch64TargetStreamer *>(OutStreamer->getTargetStreamer());
  // Do any manual lowerings.
  switch (MI->getOpcode()) {
  default:
    break;
  case AArch64::HINT: {
    // CurrentPatchableFunctionEntrySym can be CurrentFnBegin only for
    // -fpatchable-function-entry=N,0. The entry MBB is guaranteed to be
    // non-empty. If MI is the initial BTI, place the
    // __patchable_function_entries label after BTI.
    if (CurrentPatchableFunctionEntrySym &&
        CurrentPatchableFunctionEntrySym == CurrentFnBegin &&
        MI == &MF->front().front()) {
      int64_t Imm = MI->getOperand(0).getImm();
      if ((Imm & 32) && (Imm & 6)) {
        MCInst Inst;
        MCInstLowering.Lower(MI, Inst);
        EmitToStreamer(*OutStreamer, Inst);
        CurrentPatchableFunctionEntrySym = createTempSymbol("patch");
        OutStreamer->emitLabel(CurrentPatchableFunctionEntrySym);
        return;
      }
    }
    break;
  }
    case AArch64::MOVMCSym: {
      Register DestReg = MI->getOperand(0).getReg();
      const MachineOperand &MO_Sym = MI->getOperand(1);
      MachineOperand Hi_MOSym(MO_Sym), Lo_MOSym(MO_Sym);
      MCOperand Hi_MCSym, Lo_MCSym;

      Hi_MOSym.setTargetFlags(AArch64II::MO_G1 | AArch64II::MO_S);
      Lo_MOSym.setTargetFlags(AArch64II::MO_G0 | AArch64II::MO_NC);

      MCInstLowering.lowerOperand(Hi_MOSym, Hi_MCSym);
      MCInstLowering.lowerOperand(Lo_MOSym, Lo_MCSym);

      MCInst MovZ;
      MovZ.setOpcode(AArch64::MOVZXi);
      MovZ.addOperand(MCOperand::createReg(DestReg));
      MovZ.addOperand(Hi_MCSym);
      MovZ.addOperand(MCOperand::createImm(16));
      EmitToStreamer(*OutStreamer, MovZ);

      MCInst MovK;
      MovK.setOpcode(AArch64::MOVKXi);
      MovK.addOperand(MCOperand::createReg(DestReg));
      MovK.addOperand(MCOperand::createReg(DestReg));
      MovK.addOperand(Lo_MCSym);
      MovK.addOperand(MCOperand::createImm(0));
      EmitToStreamer(*OutStreamer, MovK);
      return;
  }
  case AArch64::MOVIv2d_ns:
    // It is generally beneficial to rewrite "fmov s0, wzr" to "movi d0, #0".
    // as movi is more efficient across all cores. Newer cores can eliminate
    // fmovs early and there is no difference with movi, but this not true for
    // all implementations.
    //
    // The floating-point version doesn't quite work in rare cases on older
    // CPUs, so on those targets we lower this instruction to movi.16b instead.
    if (STI->hasZeroCycleZeroingFPWorkaround() &&
        MI->getOperand(1).getImm() == 0) {
      MCInst TmpInst;
      TmpInst.setOpcode(AArch64::MOVIv16b_ns);
      TmpInst.addOperand(MCOperand::createReg(MI->getOperand(0).getReg()));
      TmpInst.addOperand(MCOperand::createImm(MI->getOperand(1).getImm()));
      EmitToStreamer(*OutStreamer, TmpInst);
      return;
    }
    break;

  case AArch64::DBG_VALUE:
  case AArch64::DBG_VALUE_LIST:
    if (isVerbose() && OutStreamer->hasRawTextSupport()) {
      SmallString<128> TmpStr;
      raw_svector_ostream OS(TmpStr);
      PrintDebugValueComment(MI, OS);
      OutStreamer->emitRawText(StringRef(OS.str()));
    }
    return;

  case AArch64::EMITBKEY: {
      ExceptionHandling ExceptionHandlingType = MAI->getExceptionHandlingType();
      if (ExceptionHandlingType != ExceptionHandling::DwarfCFI &&
          ExceptionHandlingType != ExceptionHandling::ARM)
        return;

      if (getFunctionCFISectionType(*MF) == CFISection::None)
        return;

      OutStreamer->emitCFIBKeyFrame();
      return;
  }

  case AArch64::AUT:
  case AArch64::AUTPAC: {
    const bool IsAUTPAC = MI->getOpcode() == AArch64::AUTPAC;

    // We can expand AUT/AUTPAC into 3 possible sequences:
    // - unchecked:
    //      autia x16, x0
    //      pacib x16, x1 ; if AUTPAC
    //
    // - checked and clearing:
    //      mov x17, x16
    //      autia x16, x0
    //      xpaci x17
    //      cmp x16, x17
    //      pacib x16, x1
    //      csel x16, x16, x17, eq
    //   Where we only emit the AUT if we started with an AUT.
    //
    // - checked and trapping:
    //      mov x17, x16
    //      autia x16, x0
    //      xpaci x17
    //      cmp x16, x17
    //      b.eq Lsuccess
    //      brk #<0xc470 + aut key>
    //     Lsuccess:
    //      pacib x16, x1 ; if AUTPAC
    //   Where the b.eq skips over the trap if the PAC is valid.
    //
    // This sequence is expensive, but we need more information to be able to
    // do better.
    //
    // We can't TBZ the poison bit because EnhancedPAC2 XORs the PAC bits
    // on failure.
    // We can't TST the PAC bits because we don't always know how the address
    // space is setup for the target environment (and the bottom PAC bit is
    // based on that).
    // Either way, we also don't always know whether TBI is enabled or not for
    // the specific target environment.
    //
    // FIXME: we could re-use AUTReg as a temporary register, but that would
    // require splitting the XZR cases into separate opcodes.

    // By default, auth/resign sequences check for auth failures.
    bool ShouldCheck = true;
    // In the checked sequence, we only trap if explicitly requested.
    bool ShouldTrap = MF->getFunction().hasFnAttribute("ptrauth-auth-traps");

    // However, command-line flags can override this, for experimentation.
    switch (PtrauthAuthChecks) {
    case PtrauthCheckMode::Default: break;
    case PtrauthCheckMode::Unchecked:
      ShouldCheck = ShouldTrap = false;
      break;
    case PtrauthCheckMode::Poison:
      ShouldCheck = true;
      ShouldTrap = false;
      break;
    case PtrauthCheckMode::Trap:
      ShouldCheck = ShouldTrap = true;
      break;
    }

    const auto AUTKey = (AArch64PACKey::ID)MI->getOperand(0).getImm();
    const unsigned AUTReg = MI->getOperand(1).getReg();

    const unsigned XPACOpc = getXPACOpcodeForKey(AUTKey);
    const bool AUTZero = AUTReg == AArch64::XZR;
    const unsigned AUTOpc = getAUTOpcodeForKey(AUTKey, AUTZero);

    // Checked AUTPACs and trapping AUTs need a temporary copy of the input: x17
    if ((IsAUTPAC && ShouldCheck) || ShouldTrap) {
      //  mov x17, x16
      EmitToStreamer(*OutStreamer,
        MCInstBuilder(AArch64::ORRXrs)
          .addReg(AArch64::X17)
          .addReg(AArch64::XZR)
          .addReg(AArch64::X16)
          .addImm(0));
    }

    //  autia x16, x0
    MCInst AUTInst;
    AUTInst.setOpcode(AUTOpc);
    AUTInst.addOperand(MCOperand::createReg(AArch64::X16));
    AUTInst.addOperand(MCOperand::createReg(AArch64::X16));
    if (!AUTZero)
      AUTInst.addOperand(MCOperand::createReg(AUTReg));
    EmitToStreamer(*OutStreamer, AUTInst);

    // Unchecked or checked-but-non-trapping AUT is just an "AUT": we're done.
    if (!IsAUTPAC && (!ShouldCheck || !ShouldTrap))
      return;

    // Checked sequences do an additional strip-and-compare.
    if (ShouldCheck) {
      //  xpaci x17
      EmitToStreamer(*OutStreamer,
        MCInstBuilder(XPACOpc)
          .addReg(AArch64::X17)
          .addReg(AArch64::X17));

      //  cmp x16, x17
      EmitToStreamer(*OutStreamer,
        MCInstBuilder(AArch64::SUBSXrs)
          .addReg(AArch64::XZR)
          .addReg(AArch64::X16)
          .addReg(AArch64::X17)
          .addImm(0));

      // Trapping sequences do a 'brk'.
      if (ShouldTrap) {
        //  b.eq Lsuccess
        //   where Lsuccess is encoded as 2 (the offset from this instruction to
        //   what's after the brk, divided by 4)
        EmitToStreamer(*OutStreamer,
          MCInstBuilder(AArch64::Bcc)
            .addImm(AArch64CC::EQ)
            .addImm(2));

        //  brk #<0xc470 + aut key>
        EmitToStreamer(*OutStreamer,
          MCInstBuilder(AArch64::BRK)
            .addImm(0xc470 | AUTKey));
      }
    }

    // We already emitted unchecked and checked-but-non-trapping AUTs.
    // That left us with trapping AUTs, and AUTPACs.
    // Trapping AUTs don't need PAC: we're done.
    if (!IsAUTPAC)
      return;

    const auto PACKey = (AArch64PACKey::ID)MI->getOperand(2).getImm();
    const unsigned PACReg = MI->getOperand(3).getReg();
    const bool PACZero = PACReg == AArch64::XZR;
    const unsigned PACOpc = getPACOpcodeForKey(PACKey, PACZero);

    //  pacib x16, x9
    MCInst PACInst;
    PACInst.setOpcode(PACOpc);
    PACInst.addOperand(MCOperand::createReg(AArch64::X16));
    PACInst.addOperand(MCOperand::createReg(AArch64::X16));
    if (!PACZero)
      PACInst.addOperand(MCOperand::createReg(PACReg));
    EmitToStreamer(*OutStreamer, PACInst);

    // Non-trapping AUTPAC selects the result based on the xpac check.
    // Trapping AUTPAC already trapped; unchecked AUTPAC didn't even check.
    if (ShouldTrap || !ShouldCheck)
      return;

    //  csel x16, x16, x17, eq
    EmitToStreamer(*OutStreamer,
      MCInstBuilder(AArch64::CSELXr)
        .addReg(AArch64::X16)
        .addReg(AArch64::X16)
        .addReg(AArch64::X17)
        .addImm(0));
    return;
  }
  case AArch64::EMITMTETAGGED: {
    ExceptionHandling ExceptionHandlingType = MAI->getExceptionHandlingType();
    if (ExceptionHandlingType != ExceptionHandling::DwarfCFI &&
        ExceptionHandlingType != ExceptionHandling::ARM)
      return;

    if (getFunctionCFISectionType(*MF) != CFISection::None)
      OutStreamer->emitCFIMTETaggedFrame();
    return;
  }

  // Tail calls use pseudo instructions so they have the proper code-gen
  // attributes (isCall, isReturn, etc.). We lower them to the real
  // instruction here.
  case AArch64::AUTH_TCRETURNrii:
  case AArch64::AUTH_TCRETURNriri: {
    const bool isZero = MI->getOpcode() == AArch64::AUTH_TCRETURNrii;
    const uint64_t Key = MI->getOperand(2).getImm();
    assert (Key < 2 && "Unknown key kind for authenticating tail-call return");

    const unsigned Opcodes[2][2] = {{AArch64::BRAA, AArch64::BRAAZ},
                                    {AArch64::BRAB, AArch64::BRABZ}};

    MCInst TmpInst;
    TmpInst.setOpcode(Opcodes[Key][isZero]);
    TmpInst.addOperand(MCOperand::createReg(MI->getOperand(0).getReg()));
    if (!isZero)
      TmpInst.addOperand(MCOperand::createReg(MI->getOperand(3).getReg()));
    EmitToStreamer(*OutStreamer, TmpInst);
    return;
  }
  case AArch64::TCRETURNri:
  case AArch64::TCRETURNriBTI:
  case AArch64::TCRETURNriALL: {
    MCInst TmpInst;
    TmpInst.setOpcode(AArch64::BR);
    TmpInst.addOperand(MCOperand::createReg(MI->getOperand(0).getReg()));
    EmitToStreamer(*OutStreamer, TmpInst);
    return;
  }
  case AArch64::TCRETURNdi: {
    MCOperand Dest;
    MCInstLowering.lowerOperand(MI->getOperand(0), Dest);
    MCInst TmpInst;
    TmpInst.setOpcode(AArch64::B);
    TmpInst.addOperand(Dest);
    EmitToStreamer(*OutStreamer, TmpInst);
    return;
  }
  case AArch64::SpeculationBarrierISBDSBEndBB: {
    // Print DSB SYS + ISB
    MCInst TmpInstDSB;
    TmpInstDSB.setOpcode(AArch64::DSB);
    TmpInstDSB.addOperand(MCOperand::createImm(0xf));
    EmitToStreamer(*OutStreamer, TmpInstDSB);
    MCInst TmpInstISB;
    TmpInstISB.setOpcode(AArch64::ISB);
    TmpInstISB.addOperand(MCOperand::createImm(0xf));
    EmitToStreamer(*OutStreamer, TmpInstISB);
    return;
  }
  case AArch64::SpeculationBarrierSBEndBB: {
    // Print SB
    MCInst TmpInstSB;
    TmpInstSB.setOpcode(AArch64::SB);
    EmitToStreamer(*OutStreamer, TmpInstSB);
    return;
  }
  case AArch64::TLSDESC_CALLSEQ: {
    /// lower this to:
    ///    adrp  x0, :tlsdesc:var
    ///    ldr   x1, [x0, #:tlsdesc_lo12:var]
    ///    add   x0, x0, #:tlsdesc_lo12:var
    ///    .tlsdesccall var
    ///    blr   x1
    ///    (TPIDR_EL0 offset now in x0)
    const MachineOperand &MO_Sym = MI->getOperand(0);
    MachineOperand MO_TLSDESC_LO12(MO_Sym), MO_TLSDESC(MO_Sym);
    MCOperand Sym, SymTLSDescLo12, SymTLSDesc;
    MO_TLSDESC_LO12.setTargetFlags(AArch64II::MO_TLS | AArch64II::MO_PAGEOFF);
    MO_TLSDESC.setTargetFlags(AArch64II::MO_TLS | AArch64II::MO_PAGE);
    MCInstLowering.lowerOperand(MO_Sym, Sym);
    MCInstLowering.lowerOperand(MO_TLSDESC_LO12, SymTLSDescLo12);
    MCInstLowering.lowerOperand(MO_TLSDESC, SymTLSDesc);

    MCInst Adrp;
    Adrp.setOpcode(AArch64::ADRP);
    Adrp.addOperand(MCOperand::createReg(AArch64::X0));
    Adrp.addOperand(SymTLSDesc);
    EmitToStreamer(*OutStreamer, Adrp);

    MCInst Ldr;
    if (STI->isTargetILP32()) {
      Ldr.setOpcode(AArch64::LDRWui);
      Ldr.addOperand(MCOperand::createReg(AArch64::W1));
    } else {
      Ldr.setOpcode(AArch64::LDRXui);
      Ldr.addOperand(MCOperand::createReg(AArch64::X1));
    }
    Ldr.addOperand(MCOperand::createReg(AArch64::X0));
    Ldr.addOperand(SymTLSDescLo12);
    Ldr.addOperand(MCOperand::createImm(0));
    EmitToStreamer(*OutStreamer, Ldr);

    MCInst Add;
    if (STI->isTargetILP32()) {
      Add.setOpcode(AArch64::ADDWri);
      Add.addOperand(MCOperand::createReg(AArch64::W0));
      Add.addOperand(MCOperand::createReg(AArch64::W0));
    } else {
      Add.setOpcode(AArch64::ADDXri);
      Add.addOperand(MCOperand::createReg(AArch64::X0));
      Add.addOperand(MCOperand::createReg(AArch64::X0));
    }
    Add.addOperand(SymTLSDescLo12);
    Add.addOperand(MCOperand::createImm(AArch64_AM::getShiftValue(0)));
    EmitToStreamer(*OutStreamer, Add);

    // Emit a relocation-annotation. This expands to no code, but requests
    // the following instruction gets an R_AARCH64_TLSDESC_CALL.
    MCInst TLSDescCall;
    TLSDescCall.setOpcode(AArch64::TLSDESCCALL);
    TLSDescCall.addOperand(Sym);
    EmitToStreamer(*OutStreamer, TLSDescCall);

    MCInst Blr;
    Blr.setOpcode(AArch64::BLR);
    Blr.addOperand(MCOperand::createReg(AArch64::X1));
    EmitToStreamer(*OutStreamer, Blr);

    return;
  }

  case AArch64::JumpTableDest32:
  case AArch64::JumpTableDest16:
  case AArch64::JumpTableDest8:
    LowerJumpTableDest(*OutStreamer, *MI);
    return;
  case AArch64::JumpTableAnchor: {
    int JTI = MI->getOperand(1).getIndex();
    assert(!AArch64FI->getJumpTableEntryPCRelSymbol(JTI) &&
           "unsupported compressed jump table");

    auto Label = MF->getContext().createTempSymbol();
    auto LabelE = MCSymbolRefExpr::create(Label, MF->getContext());
    AArch64FI->setJumpTableEntryInfo(JTI, 4, Label);

    OutStreamer->emitLabel(Label);
    EmitToStreamer(*OutStreamer, MCInstBuilder(AArch64::ADR)
                                     .addReg(MI->getOperand(0).getReg())
                                     .addExpr(LabelE));
    return;
  }
  
  case AArch64::FMOVH0:
  case AArch64::FMOVS0:
  case AArch64::FMOVD0:
    emitFMov0(*MI);
    return;

  case AArch64::MOPSMemoryCopyPseudo:
  case AArch64::MOPSMemoryMovePseudo:
  case AArch64::MOPSMemorySetPseudo:
  case AArch64::MOPSMemorySetTaggingPseudo:
    LowerMOPS(*OutStreamer, *MI);
    return;

  case TargetOpcode::STACKMAP:
    return LowerSTACKMAP(*OutStreamer, SM, *MI);

  case TargetOpcode::PATCHPOINT:
    return LowerPATCHPOINT(*OutStreamer, SM, *MI);

  case TargetOpcode::STATEPOINT:
    return LowerSTATEPOINT(*OutStreamer, SM, *MI);

  case TargetOpcode::FAULTING_OP:
    return LowerFAULTING_OP(*MI);

  case TargetOpcode::PATCHABLE_FUNCTION_ENTER:
    LowerPATCHABLE_FUNCTION_ENTER(*MI);
    return;

  case TargetOpcode::PATCHABLE_FUNCTION_EXIT:
    LowerPATCHABLE_FUNCTION_EXIT(*MI);
    return;

  case TargetOpcode::PATCHABLE_TAIL_CALL:
    LowerPATCHABLE_TAIL_CALL(*MI);
    return;
  case TargetOpcode::PATCHABLE_EVENT_CALL:
    return LowerPATCHABLE_EVENT_CALL(*MI, false);
  case TargetOpcode::PATCHABLE_TYPED_EVENT_CALL:
    return LowerPATCHABLE_EVENT_CALL(*MI, true);

  case AArch64::KCFI_CHECK:
    LowerKCFI_CHECK(*MI);
    return;

  case AArch64::HWASAN_CHECK_MEMACCESS:
  case AArch64::HWASAN_CHECK_MEMACCESS_SHORTGRANULES:
    LowerHWASAN_CHECK_MEMACCESS(*MI);
    return;

  case AArch64::SEH_StackAlloc:
    TS->emitARM64WinCFIAllocStack(MI->getOperand(0).getImm());
    return;

  case AArch64::SEH_SaveFPLR:
    TS->emitARM64WinCFISaveFPLR(MI->getOperand(0).getImm());
    return;

  case AArch64::SEH_SaveFPLR_X:
    assert(MI->getOperand(0).getImm() < 0 &&
           "Pre increment SEH opcode must have a negative offset");
    TS->emitARM64WinCFISaveFPLRX(-MI->getOperand(0).getImm());
    return;

  case AArch64::SEH_SaveReg:
    TS->emitARM64WinCFISaveReg(MI->getOperand(0).getImm(),
                               MI->getOperand(1).getImm());
    return;

  case AArch64::SEH_SaveReg_X:
    assert(MI->getOperand(1).getImm() < 0 &&
           "Pre increment SEH opcode must have a negative offset");
    TS->emitARM64WinCFISaveRegX(MI->getOperand(0).getImm(),
                                -MI->getOperand(1).getImm());
    return;

  case AArch64::SEH_SaveRegP:
    if (MI->getOperand(1).getImm() == 30 && MI->getOperand(0).getImm() >= 19 &&
        MI->getOperand(0).getImm() <= 28) {
      assert((MI->getOperand(0).getImm() - 19) % 2 == 0 &&
             "Register paired with LR must be odd");
      TS->emitARM64WinCFISaveLRPair(MI->getOperand(0).getImm(),
                                    MI->getOperand(2).getImm());
      return;
    }
    assert((MI->getOperand(1).getImm() - MI->getOperand(0).getImm() == 1) &&
            "Non-consecutive registers not allowed for save_regp");
    TS->emitARM64WinCFISaveRegP(MI->getOperand(0).getImm(),
                                MI->getOperand(2).getImm());
    return;

  case AArch64::SEH_SaveRegP_X:
    assert((MI->getOperand(1).getImm() - MI->getOperand(0).getImm() == 1) &&
            "Non-consecutive registers not allowed for save_regp_x");
    assert(MI->getOperand(2).getImm() < 0 &&
           "Pre increment SEH opcode must have a negative offset");
    TS->emitARM64WinCFISaveRegPX(MI->getOperand(0).getImm(),
                                 -MI->getOperand(2).getImm());
    return;

  case AArch64::SEH_SaveFReg:
    TS->emitARM64WinCFISaveFReg(MI->getOperand(0).getImm(),
                                MI->getOperand(1).getImm());
    return;

  case AArch64::SEH_SaveFReg_X:
    assert(MI->getOperand(1).getImm() < 0 &&
           "Pre increment SEH opcode must have a negative offset");
    TS->emitARM64WinCFISaveFRegX(MI->getOperand(0).getImm(),
                                 -MI->getOperand(1).getImm());
    return;

  case AArch64::SEH_SaveFRegP:
    assert((MI->getOperand(1).getImm() - MI->getOperand(0).getImm() == 1) &&
            "Non-consecutive registers not allowed for save_regp");
    TS->emitARM64WinCFISaveFRegP(MI->getOperand(0).getImm(),
                                 MI->getOperand(2).getImm());
    return;

  case AArch64::SEH_SaveFRegP_X:
    assert((MI->getOperand(1).getImm() - MI->getOperand(0).getImm() == 1) &&
            "Non-consecutive registers not allowed for save_regp_x");
    assert(MI->getOperand(2).getImm() < 0 &&
           "Pre increment SEH opcode must have a negative offset");
    TS->emitARM64WinCFISaveFRegPX(MI->getOperand(0).getImm(),
                                  -MI->getOperand(2).getImm());
    return;

  case AArch64::SEH_SetFP:
    TS->emitARM64WinCFISetFP();
    return;

  case AArch64::SEH_AddFP:
    TS->emitARM64WinCFIAddFP(MI->getOperand(0).getImm());
    return;

  case AArch64::SEH_Nop:
    TS->emitARM64WinCFINop();
    return;

  case AArch64::SEH_PrologEnd:
    TS->emitARM64WinCFIPrologEnd();
    return;

  case AArch64::SEH_EpilogStart:
    TS->emitARM64WinCFIEpilogStart();
    return;

  case AArch64::SEH_EpilogEnd:
    TS->emitARM64WinCFIEpilogEnd();
    return;

  case AArch64::SEH_PACSignLR:
    TS->emitARM64WinCFIPACSignLR();
    return;
  }

  // Finally, do the automated lowerings for everything else.
  MCInst TmpInst;
  MCInstLowering.Lower(MI, TmpInst);
  EmitToStreamer(*OutStreamer, TmpInst);
}

void AArch64AsmPrinter::emitMachOIFuncStubBody(Module &M, const GlobalIFunc &GI,
                                               MCSymbol *LazyPointer) {
  // _ifunc:
  //   adrp    x16, lazy_pointer@GOTPAGE
  //   ldr     x16, [x16, lazy_pointer@GOTPAGEOFF]
  //   ldr     x16, [x16]
  //   br      x16

  {
    MCInst Adrp;
    Adrp.setOpcode(AArch64::ADRP);
    Adrp.addOperand(MCOperand::createReg(AArch64::X16));
    MCOperand SymPage;
    MCInstLowering.lowerOperand(
        MachineOperand::CreateMCSymbol(LazyPointer,
                                       AArch64II::MO_GOT | AArch64II::MO_PAGE),
        SymPage);
    Adrp.addOperand(SymPage);
    OutStreamer->emitInstruction(Adrp, *STI);
  }

  {
    MCInst Ldr;
    Ldr.setOpcode(AArch64::LDRXui);
    Ldr.addOperand(MCOperand::createReg(AArch64::X16));
    Ldr.addOperand(MCOperand::createReg(AArch64::X16));
    MCOperand SymPageOff;
    MCInstLowering.lowerOperand(
        MachineOperand::CreateMCSymbol(LazyPointer, AArch64II::MO_GOT |
                                                        AArch64II::MO_PAGEOFF),
        SymPageOff);
    Ldr.addOperand(SymPageOff);
    Ldr.addOperand(MCOperand::createImm(0));
    OutStreamer->emitInstruction(Ldr, *STI);
  }

  OutStreamer->emitInstruction(MCInstBuilder(AArch64::LDRXui)
                                   .addReg(AArch64::X16)
                                   .addReg(AArch64::X16)
                                   .addImm(0),
                               *STI);

  OutStreamer->emitInstruction(MCInstBuilder(TM.getTargetTriple().isArm64e()
                                                 ? AArch64::BRAAZ
                                                 : AArch64::BR)
                                   .addReg(AArch64::X16),
                               *STI);
}

void AArch64AsmPrinter::emitMachOIFuncStubHelperBody(Module &M,
                                                     const GlobalIFunc &GI,
                                                     MCSymbol *LazyPointer) {
  // These stub helpers are only ever called once, so here we're optimizing for
  // minimum size by using the pre-indexed store variants, which saves a few
  // bytes of instructions to bump & restore sp.

  // _ifunc.stub_helper:
  //   stp	fp, lr, [sp, #-16]!
  //   mov	fp, sp
  //   stp	x1, x0, [sp, #-16]!
  //   stp	x3, x2, [sp, #-16]!
  //   stp	x5, x4, [sp, #-16]!
  //   stp	x7, x6, [sp, #-16]!
  //   stp	d1, d0, [sp, #-16]!
  //   stp	d3, d2, [sp, #-16]!
  //   stp	d5, d4, [sp, #-16]!
  //   stp	d7, d6, [sp, #-16]!
  //   bl	_resolver
  //   adrp	x16, lazy_pointer@GOTPAGE
  //   ldr	x16, [x16, lazy_pointer@GOTPAGEOFF]
  //   str	x0, [x16]
  //   mov	x16, x0
  //   ldp	d7, d6, [sp], #16
  //   ldp	d5, d4, [sp], #16
  //   ldp	d3, d2, [sp], #16
  //   ldp	d1, d0, [sp], #16
  //   ldp	x7, x6, [sp], #16
  //   ldp	x5, x4, [sp], #16
  //   ldp	x3, x2, [sp], #16
  //   ldp	x1, x0, [sp], #16
  //   ldp	fp, lr, [sp], #16
  //   br	x16

  OutStreamer->emitInstruction(MCInstBuilder(AArch64::STPXpre)
                                   .addReg(AArch64::SP)
                                   .addReg(AArch64::FP)
                                   .addReg(AArch64::LR)
                                   .addReg(AArch64::SP)
                                   .addImm(-2),
                               *STI);

  OutStreamer->emitInstruction(MCInstBuilder(AArch64::ADDXri)
                                   .addReg(AArch64::FP)
                                   .addReg(AArch64::SP)
                                   .addImm(0)
                                   .addImm(0),
                               *STI);

  for (int I = 0; I != 4; ++I)
    OutStreamer->emitInstruction(MCInstBuilder(AArch64::STPXpre)
                                     .addReg(AArch64::SP)
                                     .addReg(AArch64::X1 + 2 * I)
                                     .addReg(AArch64::X0 + 2 * I)
                                     .addReg(AArch64::SP)
                                     .addImm(-2),
                                 *STI);

  for (int I = 0; I != 4; ++I)
    OutStreamer->emitInstruction(MCInstBuilder(AArch64::STPDpre)
                                     .addReg(AArch64::SP)
                                     .addReg(AArch64::D1 + 2 * I)
                                     .addReg(AArch64::D0 + 2 * I)
                                     .addReg(AArch64::SP)
                                     .addImm(-2),
                                 *STI);

  OutStreamer->emitInstruction(
      MCInstBuilder(AArch64::BL)
          .addOperand(MCOperand::createExpr(lowerConstant(GI.getResolver()))),
      *STI);

  {
    MCInst Adrp;
    Adrp.setOpcode(AArch64::ADRP);
    Adrp.addOperand(MCOperand::createReg(AArch64::X16));
    MCOperand SymPage;
    MCInstLowering.lowerOperand(
        MachineOperand::CreateES(LazyPointer->getName().data() + 1,
                                 AArch64II::MO_GOT | AArch64II::MO_PAGE),
        SymPage);
    Adrp.addOperand(SymPage);
    OutStreamer->emitInstruction(Adrp, *STI);
  }

  {
    MCInst Ldr;
    Ldr.setOpcode(AArch64::LDRXui);
    Ldr.addOperand(MCOperand::createReg(AArch64::X16));
    Ldr.addOperand(MCOperand::createReg(AArch64::X16));
    MCOperand SymPageOff;
    MCInstLowering.lowerOperand(
        MachineOperand::CreateES(LazyPointer->getName().data() + 1,
                                 AArch64II::MO_GOT | AArch64II::MO_PAGEOFF),
        SymPageOff);
    Ldr.addOperand(SymPageOff);
    Ldr.addOperand(MCOperand::createImm(0));
    OutStreamer->emitInstruction(Ldr, *STI);
  }

  OutStreamer->emitInstruction(MCInstBuilder(AArch64::STRXui)
                                   .addReg(AArch64::X0)
                                   .addReg(AArch64::X16)
                                   .addImm(0),
                               *STI);

  OutStreamer->emitInstruction(MCInstBuilder(AArch64::ADDXri)
                                   .addReg(AArch64::X16)
                                   .addReg(AArch64::X0)
                                   .addImm(0)
                                   .addImm(0),
                               *STI);

  for (int I = 3; I != -1; --I)
    OutStreamer->emitInstruction(MCInstBuilder(AArch64::LDPDpost)
                                     .addReg(AArch64::SP)
                                     .addReg(AArch64::D1 + 2 * I)
                                     .addReg(AArch64::D0 + 2 * I)
                                     .addReg(AArch64::SP)
                                     .addImm(2),
                                 *STI);

  for (int I = 3; I != -1; --I)
    OutStreamer->emitInstruction(MCInstBuilder(AArch64::LDPXpost)
                                     .addReg(AArch64::SP)
                                     .addReg(AArch64::X1 + 2 * I)
                                     .addReg(AArch64::X0 + 2 * I)
                                     .addReg(AArch64::SP)
                                     .addImm(2),
                                 *STI);

  OutStreamer->emitInstruction(MCInstBuilder(AArch64::LDPXpost)
                                   .addReg(AArch64::SP)
                                   .addReg(AArch64::FP)
                                   .addReg(AArch64::LR)
                                   .addReg(AArch64::SP)
                                   .addImm(2),
                               *STI);

  OutStreamer->emitInstruction(MCInstBuilder(TM.getTargetTriple().isArm64e()
                                                 ? AArch64::BRAAZ
                                                 : AArch64::BR)
                                   .addReg(AArch64::X16),
                               *STI);
}

// Force static initialization.
extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeAArch64AsmPrinter() {
  RegisterAsmPrinter<AArch64AsmPrinter> X(getTheAArch64leTarget());
  RegisterAsmPrinter<AArch64AsmPrinter> Y(getTheAArch64beTarget());
  RegisterAsmPrinter<AArch64AsmPrinter> Z(getTheARM64Target());
  RegisterAsmPrinter<AArch64AsmPrinter> W(getTheARM64_32Target());
  RegisterAsmPrinter<AArch64AsmPrinter> V(getTheAArch64_32Target());
}
