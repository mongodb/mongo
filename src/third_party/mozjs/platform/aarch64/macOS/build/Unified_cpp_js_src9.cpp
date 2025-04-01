#define MOZ_UNIFIED_BUILD
#include "vm/BigIntType.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/BigIntType.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/BigIntType.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/BoundFunctionObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/BoundFunctionObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/BoundFunctionObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/BuildId.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/BuildId.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/BuildId.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/BuiltinObjectKind.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/BuiltinObjectKind.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/BuiltinObjectKind.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/BytecodeLocation.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/BytecodeLocation.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/BytecodeLocation.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/BytecodeUtil.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/BytecodeUtil.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/BytecodeUtil.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif