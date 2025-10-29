set(LLVM_TARGET_DEFINITIONS Options.td)
tablegen(LLVM Options.inc -gen-opt-parser-defs)
add_public_tablegen_target(PEFOptionsTableGen)

add_lld_library(lldPEF
  Driver.cpp
  Config.cpp
  InputFiles.cpp
  InputSection.cpp
  OutputSection.cpp
  Symbols.cpp
  SymbolTable.cpp

  LINK_COMPONENTS
  BinaryFormat
  Core
  MC
  Object
  Option
  Support
  TargetParser

  LINK_LIBS
  lldCommon

  DEPENDS
  PEFOptionsTableGen
  ${tablegen_deps}
  )
