#define MOZ_UNIFIED_BUILD
#include "jit/CacheIRCompiler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/CacheIRCompiler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/CacheIRCompiler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/CacheIRHealth.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/CacheIRHealth.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/CacheIRHealth.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/CacheIRSpewer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/CacheIRSpewer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/CacheIRSpewer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/CodeGenerator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/CodeGenerator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/CodeGenerator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/CompileWrappers.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/CompileWrappers.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/CompileWrappers.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/Disassemble.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Disassemble.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Disassemble.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif