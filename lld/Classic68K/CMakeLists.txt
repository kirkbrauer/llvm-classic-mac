set(LLVM_TARGET_DEFINITIONS Options.td)
tablegen(LLVM Options.inc -gen-opt-parser-defs)
add_public_tablegen_target(Classic68KOptionsTableGen)

# Generate TrapDatabase.cpp from Mac SDK Universal Interfaces headers
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(TRAP_DATABASE_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/generate_trap_database.py")
set(SDK_CINCLUDES_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../macos-classic/Interfaces&Libraries/Interfaces/CIncludes")
set(TRAP_DATABASE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/TrapDatabase.cpp")

# Find all header files to depend on (for rebuild when SDK changes)
file(GLOB SDK_HEADERS "${SDK_CINCLUDES_PATH}/*.h")

add_custom_command(
  OUTPUT ${TRAP_DATABASE_OUTPUT}
  COMMAND ${Python3_EXECUTABLE} ${TRAP_DATABASE_GENERATOR}
          "${SDK_CINCLUDES_PATH}" "${TRAP_DATABASE_OUTPUT}"
  DEPENDS ${TRAP_DATABASE_GENERATOR} ${SDK_HEADERS}
  COMMENT "Generating TrapDatabase.cpp from Mac SDK headers"
  VERBATIM
)

add_custom_target(GenerateTrapDatabase DEPENDS ${TRAP_DATABASE_OUTPUT})

add_lld_library(lldClassic68K
  Config.cpp
  Driver.cpp
  ELFReader.cpp
  ResourceWriter.cpp
  JumpTable.cpp
  CodeSegment.cpp
  DataSection.cpp
  SizeResource.cpp
  SymbolTable.cpp
  ${TRAP_DATABASE_OUTPUT}
  TrapStubs.cpp
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
  GenerateTrapDatabase
  ${tablegen_deps}
  )
