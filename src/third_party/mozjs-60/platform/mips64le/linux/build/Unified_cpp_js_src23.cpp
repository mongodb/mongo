#define MOZ_UNIFIED_BUILD
#include "jit/shared/Disassembler-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/shared/Disassembler-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/shared/Disassembler-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/shared/Lowering-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/shared/Lowering-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/shared/Lowering-shared.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "jsdate.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsdate.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsdate.cpp defines INITGUID, so it cannot be built in unified mode."
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