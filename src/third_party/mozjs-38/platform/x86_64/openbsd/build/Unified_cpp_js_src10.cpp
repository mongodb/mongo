#define MOZ_UNIFIED_BUILD
#include "vm/DateTime.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/DateTime.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/DateTime.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Debugger.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Debugger.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Debugger.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/DebuggerMemory.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/DebuggerMemory.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/DebuggerMemory.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ErrorObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ErrorObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ErrorObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ForOfIterator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ForOfIterator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ForOfIterator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/GeneratorObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/GeneratorObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/GeneratorObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/GlobalObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/GlobalObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/GlobalObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/HelperThreads.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/HelperThreads.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/HelperThreads.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Id.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Id.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Id.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Interpreter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Interpreter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Interpreter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/JSONParser.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/JSONParser.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/JSONParser.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/MemoryMetrics.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/MemoryMetrics.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/MemoryMetrics.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Monitor.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Monitor.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Monitor.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/NativeObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/NativeObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/NativeObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ObjectGroup.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ObjectGroup.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ObjectGroup.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/PIC.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/PIC.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/PIC.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif