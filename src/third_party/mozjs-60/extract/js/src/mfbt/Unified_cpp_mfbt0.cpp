#define MOZ_UNIFIED_BUILD
#include "Assertions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "Assertions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "Assertions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "ChaosMode.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "ChaosMode.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "ChaosMode.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "FloatingPoint.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "FloatingPoint.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "FloatingPoint.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "HashFunctions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "HashFunctions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "HashFunctions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "JSONWriter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "JSONWriter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "JSONWriter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "Poison.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "Poison.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "Poison.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "SHA1.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "SHA1.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "SHA1.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "TaggedAnonymousMemory.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "TaggedAnonymousMemory.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "TaggedAnonymousMemory.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "Unused.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "Unused.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "Unused.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/double-conversion/bignum-dtoa.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/double-conversion/bignum-dtoa.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/double-conversion/bignum-dtoa.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/double-conversion/bignum.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/double-conversion/bignum.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/double-conversion/bignum.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/double-conversion/cached-powers.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/double-conversion/cached-powers.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/double-conversion/cached-powers.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/double-conversion/diy-fp.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/double-conversion/diy-fp.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/double-conversion/diy-fp.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/double-conversion/double-conversion.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/double-conversion/double-conversion.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/double-conversion/double-conversion.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/double-conversion/fast-dtoa.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/double-conversion/fast-dtoa.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/double-conversion/fast-dtoa.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/double-conversion/fixed-dtoa.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/double-conversion/fixed-dtoa.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/double-conversion/fixed-dtoa.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif