#define MOZ_UNIFIED_BUILD
#include "devtools/sharkctl.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "devtools/sharkctl.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "devtools/sharkctl.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "frontend/BytecodeCompiler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/BytecodeCompiler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/BytecodeCompiler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/BytecodeEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/BytecodeEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/BytecodeEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/FoldConstants.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/FoldConstants.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/FoldConstants.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/NameFunctions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/NameFunctions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/NameFunctions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/ParseMaps.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/ParseMaps.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/ParseMaps.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/ParseNode.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/ParseNode.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/ParseNode.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/TokenStream.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/TokenStream.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/TokenStream.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Barrier.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Barrier.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Barrier.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/GCTrace.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/GCTrace.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/GCTrace.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Iteration.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Iteration.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Iteration.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Marking.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Marking.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Marking.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Memory.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Memory.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Memory.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Nursery.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Nursery.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Nursery.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/RootMarking.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/RootMarking.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/RootMarking.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif