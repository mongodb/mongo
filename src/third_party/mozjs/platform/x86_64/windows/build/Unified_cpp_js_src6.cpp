#define MOZ_UNIFIED_BUILD
#include "builtin/streams/StreamAPI.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/StreamAPI.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/StreamAPI.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/TeeState.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/TeeState.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/TeeState.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/WritableStream.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/WritableStream.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/WritableStream.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/WritableStreamDefaultController.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/WritableStreamDefaultController.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/WritableStreamDefaultController.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/WritableStreamDefaultControllerOperations.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/WritableStreamDefaultControllerOperations.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/WritableStreamDefaultControllerOperations.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/WritableStreamDefaultWriter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/WritableStreamDefaultWriter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/WritableStreamDefaultWriter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif