#define MOZ_UNIFIED_BUILD
#include "builtin/intl/PluralRules.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/intl/PluralRules.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/intl/PluralRules.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/intl/RelativeTimeFormat.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/intl/RelativeTimeFormat.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/intl/RelativeTimeFormat.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/intl/SharedIntlData.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/intl/SharedIntlData.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/intl/SharedIntlData.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "devtools/sharkctl.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "devtools/sharkctl.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "devtools/sharkctl.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "ds/Bitmap.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "ds/Bitmap.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "ds/Bitmap.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "ds/LifoAlloc.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "ds/LifoAlloc.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "ds/LifoAlloc.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif