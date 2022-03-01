#define MOZ_UNIFIED_BUILD
#include "debugger/DebugScript.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "debugger/DebugScript.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "debugger/DebugScript.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "debugger/Debugger.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "debugger/Debugger.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "debugger/Debugger.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "debugger/DebuggerMemory.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "debugger/DebuggerMemory.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "debugger/DebuggerMemory.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "debugger/Environment.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "debugger/Environment.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "debugger/Environment.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "debugger/Frame.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "debugger/Frame.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "debugger/Frame.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "debugger/NoExecute.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "debugger/NoExecute.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "debugger/NoExecute.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif