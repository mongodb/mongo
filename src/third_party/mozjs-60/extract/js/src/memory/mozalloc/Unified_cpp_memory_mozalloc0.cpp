#define MOZ_UNIFIED_BUILD
#include "/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/memory/mozalloc/mozalloc.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/memory/mozalloc/mozalloc.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/memory/mozalloc/mozalloc.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/memory/mozalloc/mozalloc_oom.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/memory/mozalloc/mozalloc_oom.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/memory/mozalloc/mozalloc_oom.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif