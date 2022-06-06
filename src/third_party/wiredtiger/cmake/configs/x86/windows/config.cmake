set(WT_ARCH "x86" CACHE STRING "")
set(WT_OS "windows" CACHE STRING "")
set(WT_POSIX OFF CACHE BOOL "")
set(SPINLOCK_TYPE "msvc" CACHE STRING "" FORCE)
# We force a static compilation to generate a ".lib" file. We can then
# additionally generate a dll file using a *DEF file.
set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(WITH_PIC ON CACHE BOOL "" FORCE)

# Inline expansion.
add_compile_options(/Ob1)
# Enable string pooling.
add_compile_options(/GF)
# Extern "C" does not throw.
add_compile_options(/EHsc)
# Separate functions for linker.
add_compile_options(/Gy)
# Conformance: wchar_t is a native type, not a typedef.
add_compile_options(/Zc:wchar_t)
# Use the __cdecl calling convention for all functions.
add_compile_options(/Gd)
# Ignore deprecated functions.
add_compile_options(/wd4996)
# Ignore warning about mismatched const qualifiers.
add_compile_options(/wd4090)

# Disable incremental linking.
string(APPEND win_link_flags " /INCREMENTAL:NO")
# Remove dead code.
string(APPEND win_link_flags " /OPT:REF")
# Allow executables to be randomly rebased at load time (enables virtual address allocation randomization).
string(APPEND win_link_flags " /DYNAMICBASE")
# Executable is compatible with the Windows Data Execution Prevention.
string(APPEND win_link_flags " /NXCOMPAT")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${win_link_flags}")
