//===--- BackendUtil.h - LLVM Backend Utilities -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_CODEGEN_BACKENDUTIL_H
#define LLVM_CLANG_CODEGEN_BACKENDUTIL_H

#include "clang/Basic/LLVM.h"
#include "llvm/IR/ModuleSummaryIndex.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <memory>

namespace llvm {
  class BitcodeModule;
  template <typename T> class Expected;
  template <typename T> class IntrusiveRefCntPtr;
  class Module;
  class MemoryBufferRef;
  namespace vfs {
  class FileSystem;
  } // namespace vfs
}

namespace clang {
  class DiagnosticsEngine;
  class HeaderSearchOptions;
  class CodeGenOptions;
  class TargetOptions;
  class LangOptions;
  class CASOptions; // MCCAS
  class BackendConsumer;

  enum BackendAction {
    Backend_EmitAssembly,  ///< Emit native assembly files
    Backend_EmitBC,        ///< Emit LLVM bitcode files
    Backend_EmitLL,        ///< Emit human-readable LLVM assembly
    Backend_EmitNothing,   ///< Don't emit anything (benchmarking mode)
    Backend_EmitMCNull,    ///< Run CodeGen, but don't emit anything
    Backend_EmitObj        ///< Emit native object files
  };

  void EmitBackendOutput(DiagnosticsEngine &Diags, const HeaderSearchOptions &,
                         const CodeGenOptions &CGOpts,
                         const TargetOptions &TOpts, const LangOptions &LOpts,
                         const CASOptions &CASOpts, // MCCAS
                         StringRef TDesc, llvm::Module *M, BackendAction Action,
                         llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
                         std::unique_ptr<raw_pwrite_stream> OS,
                         std::unique_ptr<raw_pwrite_stream> CasidOS = nullptr,
                         BackendConsumer *BC = nullptr);

  void EmbedBitcode(llvm::Module *M, const CodeGenOptions &CGOpts,
                    llvm::MemoryBufferRef Buf);

  void EmbedObject(llvm::Module *M, const CodeGenOptions &CGOpts,
                   DiagnosticsEngine &Diags);
}

#endif
