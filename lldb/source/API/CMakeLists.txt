## BEGIN SWIFT
set(LLVM_NO_RTTI 1)

include(AddLLVM)

if (LLDB_ENABLE_SWIFT_SUPPORT)
  include(SwiftAddCustomCommandTarget)
endif()
## END SWIFT

get_property(LLDB_ALL_PLUGINS GLOBAL PROPERTY LLDB_PLUGINS)

if(LLDB_BUILD_FRAMEWORK)
  set(option_install_prefix INSTALL_PREFIX ${LLDB_FRAMEWORK_INSTALL_DIR})
  set(option_framework FRAMEWORK)
endif()

if(LLDB_ENABLE_PYTHON)
  if (APPLE AND LLVM_ENABLE_LOCAL_SUBMODULE_VISIBILITY)
    # Work around an issue with the Python headers, which have a modular include
    # inside an extern "C" block.
    remove_module_flags()
  endif()
  get_target_property(python_bindings_dir swig_wrapper_python BINARY_DIR)
  set(lldb_python_wrapper ${python_bindings_dir}/LLDBWrapPython.cpp)
endif()

if(LLDB_ENABLE_LUA)
  get_target_property(lua_bindings_dir swig_wrapper_lua BINARY_DIR)
  set(lldb_lua_wrapper ${lua_bindings_dir}/LLDBWrapLua.cpp)
endif()

add_lldb_library(liblldb SHARED ${option_framework}
  SBAddress.cpp
  SBAttachInfo.cpp
  SBBlock.cpp
  SBBreakpoint.cpp
  SBBreakpointLocation.cpp
  SBBreakpointName.cpp
  SBBreakpointOptionCommon.cpp
  SBBroadcaster.cpp
  SBCommandInterpreter.cpp
  SBCommandInterpreterRunOptions.cpp
  SBCommandReturnObject.cpp
  SBCommunication.cpp
  SBCompileUnit.cpp
  SBData.cpp
  SBDebugger.cpp
  SBDeclaration.cpp
  SBEnvironment.cpp
  SBError.cpp
  SBEvent.cpp
  SBExecutionContext.cpp
  SBExpressionOptions.cpp
  SBFileSpec.cpp
  SBFile.cpp
  SBFileSpecList.cpp
  SBFormat.cpp
  SBFrame.cpp
  SBFunction.cpp
  SBHostOS.cpp
  SBInstruction.cpp
  SBInstructionList.cpp
  SBLanguageRuntime.cpp
  SBLaunchInfo.cpp
  SBLineEntry.cpp
  SBListener.cpp
  SBMemoryRegionInfo.cpp
  SBMemoryRegionInfoList.cpp
  SBModule.cpp
  SBModuleSpec.cpp
  SBPlatform.cpp
  SBProcess.cpp
  SBProcessInfo.cpp
  SBProcessInfoList.cpp
  SBQueue.cpp
  SBQueueItem.cpp
  SBReproducer.cpp
  SBScriptObject.cpp
  SBSection.cpp
  SBSourceManager.cpp
  SBStream.cpp
  SBStringList.cpp
  SBStructuredData.cpp
  SBSymbol.cpp
  SBSymbolContext.cpp
  SBSymbolContextList.cpp
  SBTarget.cpp
  SBThread.cpp
  SBThreadCollection.cpp
  SBThreadPlan.cpp
  SBTrace.cpp
  SBTraceCursor.cpp
  SBType.cpp
  SBTypeCategory.cpp
  SBTypeEnumMember.cpp
  SBTypeFilter.cpp
  SBTypeFormat.cpp
  SBTypeNameSpecifier.cpp
  SBTypeSummary.cpp
  SBTypeSynthetic.cpp
  SBValue.cpp
  SBValueList.cpp
  SBVariablesOptions.cpp
  SBWatchpoint.cpp
  SBWatchpointOptions.cpp
  SBUnixSignals.cpp
  SystemInitializerFull.cpp
  ${lldb_python_wrapper}
  ${lldb_lua_wrapper}

  LINK_LIBS
    lldbBreakpoint
    lldbCore
    lldbDataFormatters
    lldbExpression
    lldbHost
    lldbInitialization
    lldbInterpreter
    lldbSymbol
    lldbTarget
    lldbUtility
    lldbVersion
    ${LLDB_ALL_PLUGINS}
## BEGIN SWIFT
    ${SWIFT_ALL_LIBS}
## END SWIFT
  LINK_COMPONENTS
    Support

  ${option_install_prefix}
)

# lib/pythonX.Y/dist-packages/lldb/_lldb.so is a symlink to lib/liblldb.so,
# which depends on lib/libLLVM*.so (BUILD_SHARED_LIBS) or lib/libLLVM-10git.so
# (LLVM_LINK_LLVM_DYLIB). Add an additional rpath $ORIGIN/../../../../lib so
# that _lldb.so can be loaded from Python.
if(LLDB_ENABLE_PYTHON AND (BUILD_SHARED_LIBS OR LLVM_LINK_LLVM_DYLIB) AND UNIX AND NOT APPLE)
  set_property(TARGET liblldb APPEND PROPERTY INSTALL_RPATH "\$ORIGIN/../../../../lib${LLVM_LIBDIR_SUFFIX}")
endif()

if(Python3_RPATH)
  lldb_setup_rpaths(liblldb
    BUILD_RPATH
      "${Python3_RPATH}"
    INSTALL_RPATH
      "@loader_path/../../../../../../../../Library/Frameworks/"
      "@loader_path/../../../../../Developer/Library/Frameworks/"
      "@loader_path/../../../../Developer/Library/Frameworks/"
      "@loader_path/../../../../Frameworks"
      "@loader_path/../../../"
      "/Library/Developer/CommandLineTools/Library/Frameworks"
      "/Applications/Xcode.app/Contents/Developer/Library/Frameworks/"
    )
endif()

# BEGIN Swift Mods
# Note that add_properties_for_swift_modules appends RPATHs so it's critical
# that this is called after lldb_setup_rpaths.
if(LLDB_BUILD_FRAMEWORK)
  add_properties_for_swift_modules(liblldb "../../../../../../usr/" "../../../../")
else()
  add_properties_for_swift_modules(liblldb "../")
endif()
# END Swift Mods

if(LLDB_ENABLE_PYTHON)
  add_dependencies(liblldb swig_wrapper_python)

  if (MSVC)
    set_property(SOURCE ${lldb_python_wrapper} APPEND_STRING PROPERTY COMPILE_FLAGS " /W0")
  else()
    set_property(SOURCE ${lldb_python_wrapper} APPEND_STRING PROPERTY COMPILE_FLAGS " -w")
  endif()

  set_source_files_properties(${lldb_python_wrapper} PROPERTIES GENERATED ON)
  if (CLANG_CL)
    set_property(SOURCE ${lldb_python_wrapper} APPEND_STRING
      PROPERTY COMPILE_FLAGS " -Wno-unused-function")
  endif()
  if (LLVM_COMPILER_IS_GCC_COMPATIBLE AND
      NOT "${CMAKE_SYSTEM_NAME}" MATCHES "Darwin")
    set_property(SOURCE ${lldb_python_wrapper} APPEND_STRING
      PROPERTY COMPILE_FLAGS " -Wno-sequence-point -Wno-cast-qual")
  endif ()
endif()

if(LLDB_ENABLE_LUA)
  add_dependencies(liblldb swig_wrapper_lua)
  target_include_directories(liblldb PRIVATE ${LUA_INCLUDE_DIR})

  if (MSVC)
    set_property(SOURCE ${lldb_lua_wrapper} APPEND_STRING PROPERTY COMPILE_FLAGS " /W0")
  else()
    set_property(SOURCE ${lldb_lua_wrapper} APPEND_STRING PROPERTY COMPILE_FLAGS " -w")
  endif()

  set_source_files_properties(${lldb_lua_wrapper} PROPERTIES GENERATED ON)
endif()

set_target_properties(liblldb
  PROPERTIES
  VERSION ${LLDB_VERSION}
)

target_compile_definitions(liblldb PRIVATE LLDB_IN_LIBLLDB)
if (NOT CMAKE_SYSTEM_NAME MATCHES "Windows")
  if (NOT LLDB_EXPORT_ALL_SYMBOLS)
    # If we're not exporting all symbols, we'll want to explicitly set
    # the exported symbols here.  This prevents 'log enable --stack ...'
    # from working on some systems but limits the liblldb size.
    MESSAGE("-- Symbols (liblldb): exporting all symbols from the lldb namespace")
    add_llvm_symbol_exports(liblldb ${CMAKE_CURRENT_SOURCE_DIR}/liblldb.exports)
  elseif (NOT LLDB_EXPORT_ALL_SYMBOLS_EXPORTS_FILE)
    # Don't use an explicit export. Instead, tell the linker to export all symbols.
    MESSAGE("-- Symbols (liblldb): exporting all symbols from the lldb and lldb_private namespaces")
    MESSAGE(WARNING "Private LLDB symbols frequently change and no API stability is guaranteed. "
                    "Only the SB API is guaranteed to be stable.")
    add_llvm_symbol_exports(liblldb ${CMAKE_CURRENT_SOURCE_DIR}/liblldb-private.exports)
  else ()
    MESSAGE("-- Symbols (liblldb): exporting all symbols specified in the exports "
            " file '${LLDB_EXPORT_ALL_SYMBOLS_EXPORTS_FILE}'")
    MESSAGE(WARNING "Private LLDB symbols frequently change and no API stability is guaranteed. "
                    "Only the SB API is guaranteed to be stable.")
    add_llvm_symbol_exports(liblldb "${LLDB_EXPORT_ALL_SYMBOLS_EXPORTS_FILE}")
  endif()
  set_target_properties(liblldb_exports PROPERTIES FOLDER "lldb misc")
elseif (LLDB_EXPORT_ALL_SYMBOLS)
  MESSAGE("-- Symbols (liblldb): exporting all symbols from the lldb and lldb_private namespaces")

  # Pull out the various lldb libraries linked into liblldb, these will be used
  # when looking for symbols to extract. We ignore most plugin libraries here,
  # because we may expose more symbols than the DLL limit and these symbols
  # aren't useful to expose.
  get_target_property(all_liblldb_libs liblldb LINK_LIBRARIES)
  set(lldb_libs "")
  foreach(lib ${all_liblldb_libs})
    if(TARGET ${lib} AND ${lib} MATCHES "^lldb" AND
       (${lib} IN_LIST LLDB_EXPORT_ALL_SYMBOLS_PLUGINS OR
        NOT ${lib} MATCHES "^lldbPlugin"))
      get_target_property(lib_type ${lib} TYPE)
      if("${lib_type}" STREQUAL "STATIC_LIBRARY")
        list(APPEND lldb_libs ${lib})
      endif()
    endif()
  endforeach(lib)
  list(REMOVE_DUPLICATES lldb_libs)

  # Extract all of the private symbols and produce a single file we can use for
  # the exports.
  set(exported_symbol_file ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/liblldb_private.symbols)
  get_host_tool_path(llvm-nm LLVM_NM llvm_nm_exe llvm_nm_target)
  add_custom_command(
    OUTPUT ${exported_symbol_file}
    COMMAND "${Python3_EXECUTABLE}"
      ${LLDB_SOURCE_DIR}/scripts/msvc_extract_private_symbols.py
      ${lldb_libs} -o ${exported_symbol_file} --nm=${llvm_nm_exe}
    WORKING_DIRECTORY ${LLVM_LIBRARY_OUTPUT_INTDIR}
    DEPENDS ${LLDB_SOURCE_DIR}/scripts/msvc_extract_private_symbols.py
      ${lldb_libs} ${llvm_nm_target}
    VERBATIM
    COMMENT "Generating liblldb private export list"
  )

  add_llvm_symbol_exports(liblldb ${exported_symbol_file})
  set_target_properties(liblldb_exports PROPERTIES FOLDER "lldb misc")
endif()

if (NOT MSVC)
  set_target_properties(liblldb
    PROPERTIES
    OUTPUT_NAME lldb
  )
endif()

# The Clang expression parser in LLDB requires the Clang resource directory to function.
if (TARGET clang-resource-headers)
  # If building alongside Clang, just add a dependency to ensure it is build together with liblldb.
  add_dependencies(liblldb clang-resource-headers)
else()
  # In a standalone build create a symlink from the LLDB library directory that points to the
  # resource directory in the Clang library directory. LLDB searches relative to its install path,
  # and the symlink is created in the same relative path as the resource directory of Clang when
  # building alongside Clang.
  # When building the LLDB framework, this isn't necessary as there we copy everything we need into
  # the framework (including the Clang resourece directory).
  if(NOT LLDB_BUILD_FRAMEWORK)
    set(LLDB_CLANG_RESOURCE_DIR "$<TARGET_FILE_DIR:liblldb>/clang")
    add_custom_command(TARGET liblldb POST_BUILD
      COMMENT "Linking Clang resource dir into LLDB build directory: ${LLDB_CLANG_RESOURCE_DIR}"
      COMMAND ${CMAKE_COMMAND} -E create_symlink
      "${LLDB_EXTERNAL_CLANG_RESOURCE_DIR}" "${LLDB_CLANG_RESOURCE_DIR}"
    )
  endif()
endif()

if(LLDB_BUILD_FRAMEWORK)
  include(LLDBFramework)

  if (TARGET install-liblldb)
    add_dependencies(install-liblldb
      lldb-framework-cleanup)
  endif()

  if (TARGET install-liblldb-stripped)
    add_dependencies(install-liblldb-stripped
      lldb-framework-cleanup)
  endif()
endif()

## BEGIN SWIFT
set(lib_dir "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib${LLVM_LIBDIR_SUFFIX}")

if(EXISTS ${SWIFT_BINARY_DIR}/lib${LLVM_LIBDIR_SUFFIX}/swift/clang)
  set(CLANG_RESOURCE_PATH "${SWIFT_BINARY_DIR}/lib${LLVM_LIBDIR_SUFFIX}/swift/clang")
elseif(LLDB_BUILT_STANDALONE)
  set(CLANG_RESOURCE_PATH ${LLDB_EXTERNAL_CLANG_RESOURCE_DIR})
elseif(LLVM_LIBRARY_OUTPUT_INTDIR)
  set(CLANG_RESOURCE_PATH "${LLVM_LIBRARY_OUTPUT_INTDIR}/swift/clang")
else()
  message(SEND_ERROR "unified build cannot locate clang resource directory")
endif()

set(clang_headers_target ${CLANG_RESOURCE_PATH}/include)
set(SWIFT_RESOURCE_PATH "${SWIFT_BINARY_DIR}/lib${LLVM_LIBDIR_SUFFIX}/swift")
if(NOT LLDB_BUILT_STANDALONE)
  set(clang_headers_target clang-resource-headers symlink_clang_headers)
endif()

# Copy the clang resource directory.
if (LLDB_ENABLE_SWIFT_SUPPORT)
add_custom_command_target(
    copy-clang-resource-dir
    COMMAND "${CMAKE_COMMAND}" "-E" "copy_directory" "${CLANG_RESOURCE_PATH}" "${lib_dir}/lldb/clang/"
    OUTPUT "${lib_dir}/lldb/clang/"
    VERBATIM
    ALL
    DEPENDS ${clang_headers_target})

# Only copy the swift resource directory if you are building lldb standalone.
# Otherwise, just rely on the swift libdir available from the swift build.
if(LLDB_BUILT_STANDALONE)
  add_custom_command_target(
      copy-swift-resource-dir
      COMMAND "${CMAKE_COMMAND}" "-E" "copy_directory" "${SWIFT_RESOURCE_PATH}" "${lib_dir}/lldb/swift/"
      OUTPUT "${lib_dir}/lldb/swift/"
      VERBATIM
      ALL
      DEPENDS ${SWIFT_RESOURCE_PATH})
endif()

add_dependencies(liblldb ${copy-clang-resource-dir} ${copy-swift-resource-dir})
endif()

install(
  CODE "file(MAKE_DIRECTORY ${lib_dir}/lldb)")

install(
  DIRECTORY "${lib_dir}/lldb/clang"
  DESTINATION lib${LLVM_LIBDIR_SUFFIX}/lldb/)

if(LLDB_ENABLE_SWIFT_SUPPORT)
if(LLDB_BUILT_STANDALONE)
  install(
    DIRECTORY "${lib_dir}/lldb/swift"
    DESTINATION lib${LLVM_LIBDIR_SUFFIX}/lldb/)
endif()
endif()
## END SWIFT
