#define MOZ_UNIFIED_BUILD
#include "builtin/streams/PipeToState.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/PipeToState.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/PipeToState.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/PullIntoDescriptor.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/PullIntoDescriptor.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/PullIntoDescriptor.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/QueueWithSizes.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/QueueWithSizes.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/QueueWithSizes.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/QueueingStrategies.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/QueueingStrategies.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/QueueingStrategies.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/ReadableStream.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/ReadableStream.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/ReadableStream.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/streams/ReadableStreamBYOBReader.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/streams/ReadableStreamBYOBReader.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/streams/ReadableStreamBYOBReader.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif