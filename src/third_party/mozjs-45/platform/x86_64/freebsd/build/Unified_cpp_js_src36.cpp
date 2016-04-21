#define MOZ_UNIFIED_BUILD
#include "vm/WeakMapPtr.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/WeakMapPtr.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/WeakMapPtr.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Xdr.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Xdr.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Xdr.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
