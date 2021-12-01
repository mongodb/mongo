#define MOZ_UNIFIED_BUILD
#include "threading/windows/CpuCount.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "threading/windows/CpuCount.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "threading/windows/CpuCount.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "threading/windows/Thread.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "threading/windows/Thread.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "threading/windows/Thread.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "util/AllocPolicy.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "util/AllocPolicy.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "util/AllocPolicy.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "util/NativeStack.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "util/NativeStack.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "util/NativeStack.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
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