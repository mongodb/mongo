#define MOZ_UNIFIED_BUILD
#include "builtin/WeakSetObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/WeakSetObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/WeakSetObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/intl/Collator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/intl/Collator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/intl/Collator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/intl/CommonFunctions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/intl/CommonFunctions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/intl/CommonFunctions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/intl/DateTimeFormat.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/intl/DateTimeFormat.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/intl/DateTimeFormat.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/intl/IntlObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/intl/IntlObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/intl/IntlObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/intl/NumberFormat.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/intl/NumberFormat.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/intl/NumberFormat.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif