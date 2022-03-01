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
#include "Compression.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "Compression.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "Compression.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "RandomNum.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "RandomNum.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "RandomNum.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "UniquePtrExtensions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "UniquePtrExtensions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "UniquePtrExtensions.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "Utf8.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "Utf8.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "Utf8.cpp defines INITGUID, so it cannot be built in unified mode."
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