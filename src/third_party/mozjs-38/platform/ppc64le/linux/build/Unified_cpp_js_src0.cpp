#define MOZ_UNIFIED_BUILD
#include "asmjs/AsmJSFrameIterator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "asmjs/AsmJSFrameIterator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "asmjs/AsmJSFrameIterator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "asmjs/AsmJSLink.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "asmjs/AsmJSLink.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "asmjs/AsmJSLink.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "asmjs/AsmJSModule.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "asmjs/AsmJSModule.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "asmjs/AsmJSModule.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "asmjs/AsmJSSignalHandlers.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "asmjs/AsmJSSignalHandlers.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "asmjs/AsmJSSignalHandlers.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "asmjs/AsmJSValidate.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "asmjs/AsmJSValidate.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "asmjs/AsmJSValidate.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/AtomicsObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/AtomicsObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/AtomicsObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/Eval.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/Eval.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/Eval.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/Intl.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/Intl.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/Intl.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/MapObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/MapObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/MapObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/Object.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/Object.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/Object.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/Profilers.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/Profilers.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/Profilers.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/SIMD.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/SIMD.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/SIMD.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/SymbolObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/SymbolObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/SymbolObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/TestingFunctions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/TestingFunctions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/TestingFunctions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/TypedObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/TypedObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/TypedObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/WeakSetObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/WeakSetObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/WeakSetObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif