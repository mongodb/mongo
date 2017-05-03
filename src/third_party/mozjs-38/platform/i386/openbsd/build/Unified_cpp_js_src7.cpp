#define MOZ_UNIFIED_BUILD
#include "jit/x86/Assembler-x86.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86/Assembler-x86.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86/Assembler-x86.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x86/Bailouts-x86.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86/Bailouts-x86.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86/Bailouts-x86.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x86/BaselineCompiler-x86.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86/BaselineCompiler-x86.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86/BaselineCompiler-x86.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x86/BaselineIC-x86.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86/BaselineIC-x86.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86/BaselineIC-x86.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x86/CodeGenerator-x86.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86/CodeGenerator-x86.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86/CodeGenerator-x86.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x86/Lowering-x86.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86/Lowering-x86.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86/Lowering-x86.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x86/MacroAssembler-x86.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86/MacroAssembler-x86.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86/MacroAssembler-x86.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x86/Trampoline-x86.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86/Trampoline-x86.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86/Trampoline-x86.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsalloc.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsalloc.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsalloc.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsapi.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsapi.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsapi.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsbool.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsbool.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsbool.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jscntxt.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jscntxt.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jscntxt.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jscompartment.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jscompartment.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jscompartment.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsdate.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsdate.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsdate.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsdtoa.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsdtoa.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsdtoa.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsexn.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsexn.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsexn.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif