#define MOZ_UNIFIED_BUILD
#include "builtin/streams/ReadableStreamDefaultController.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/ReadableStreamDefaultController.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/ReadableStreamDefaultController.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/ReadableStreamDefaultControllerOperations.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/ReadableStreamDefaultControllerOperations.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/ReadableStreamDefaultControllerOperations.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/ReadableStreamDefaultReader.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/ReadableStreamDefaultReader.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/ReadableStreamDefaultReader.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/ReadableStreamInternals.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/ReadableStreamInternals.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/ReadableStreamInternals.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/ReadableStreamOperations.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/ReadableStreamOperations.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/ReadableStreamOperations.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/ReadableStreamReader.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/ReadableStreamReader.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/ReadableStreamReader.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif