#define MOZ_UNIFIED_BUILD
#include "double-conversion/double-conversion/double-to-string.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/double-conversion/double-to-string.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/double-conversion/double-to-string.cc defines INITGUID, so it cannot be built in unified mode."
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
#include "double-conversion/double-conversion/string-to-double.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/double-conversion/string-to-double.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/double-conversion/string-to-double.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/double-conversion/strtod.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/double-conversion/strtod.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/double-conversion/strtod.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif