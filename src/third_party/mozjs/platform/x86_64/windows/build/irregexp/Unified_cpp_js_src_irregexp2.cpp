#define MOZ_UNIFIED_BUILD
#include "irregexp/util/UnicodeShim.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/util/UnicodeShim.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/util/UnicodeShim.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif