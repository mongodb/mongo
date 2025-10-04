
# Find the armasm assembler, armasm or armasm64

set(ASM_DIALECT "_ARMASM")

# if we are using the 64bit cl compiler, assume we also want the 64bit assembler
if(";${CMAKE_VS_PLATFORM_NAME};${MSVC_C_ARCHITECTURE_ID};${MSVC_CXX_ARCHITECTURE_ID};"
  MATCHES ";ARM64;")
  set(CMAKE_ASM${ASM_DIALECT}_COMPILER_INIT armasm64)
else()
  set(CMAKE_ASM${ASM_DIALECT}_COMPILER_INIT armasm)
endif()

include(CMakeDetermineASMCompiler)
set(ASM_DIALECT)
