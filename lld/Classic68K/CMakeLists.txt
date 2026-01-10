set(LLVM_TARGET_DEFINITIONS Options.td)
tablegen(LLVM Options.inc -gen-opt-parser-defs)
add_public_tablegen_target(Classic68KOptionsTableGen)

add_lld_library(lldClassic68K
  Config.cpp
  Driver.cpp
  ELFReader.cpp
  ResourceWriter.cpp
  JumpTable.cpp
  CodeSegment.cpp
  DataSection.cpp
  SizeResource.cpp
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
  Classic68KOptionsTableGen
  ${tablegen_deps}
  )
