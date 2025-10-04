#define MOZ_UNIFIED_BUILD
#include "util/Printf.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "util/Printf.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "util/Printf.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "util/StringBuffer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "util/StringBuffer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "util/StringBuffer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "util/StructuredSpewer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "util/StructuredSpewer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "util/StructuredSpewer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "util/Text.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "util/Text.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "util/Text.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "util/Unicode.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "util/Unicode.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "util/Unicode.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif