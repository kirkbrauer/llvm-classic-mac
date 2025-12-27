set(LLVM_TARGET_DEFINITIONS Options.td)
tablegen(LLVM Options.inc -gen-opt-parser-defs)
add_public_tablegen_target(PEFOptionsTableGen)

add_lld_library(lldPEF
  Driver.cpp
  Config.cpp
  ELFInputFiles.cpp
  InputFiles.cpp
  InputSection.cpp
  XCOFFInputFiles.cpp
  MarkLive.cpp
  OutputSection.cpp
  PatternEncoder.cpp
  RelocWriter.cpp
  Relocations.cpp
  Symbols.cpp
  SymbolTable.cpp
  Writer.cpp

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
