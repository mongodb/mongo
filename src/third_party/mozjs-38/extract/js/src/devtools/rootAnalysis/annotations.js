/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

"use strict";

// Ignore calls made through these function pointers
var ignoreIndirectCalls = {
    "mallocSizeOf" : true,
    "aMallocSizeOf" : true,
    "_malloc_message" : true,
    "je_malloc_message" : true,
    "chunk_dalloc" : true,
    "chunk_alloc" : true,
    "__conv" : true,
    "__convf" : true,
    "prerrortable.c:callback_newtable" : true,
    "mozalloc_oom.cpp:void (* gAbortHandler)(size_t)" : true
};

function indirectCallCannotGC(fullCaller, fullVariable)
{
    var caller = readable(fullCaller);

    // This is usually a simple variable name, but sometimes a full name gets
    // passed through. And sometimes that name is truncated. Examples:
    //   _ZL13gAbortHandler|mozalloc_oom.cpp:void (* gAbortHandler)(size_t)
    //   _ZL14pMutexUnlockFn|umutex.cpp:void (* pMutexUnlockFn)(const void*
    var name = readable(fullVariable);

    if (name in ignoreIndirectCalls)
        return true;

    if (name == "mapper" && caller == "ptio.c:pt_MapError")
        return true;

    if (name == "params" && caller == "PR_ExplodeTime")
        return true;

    if (name == "op" && /GetWeakmapKeyDelegate/.test(caller))
        return true;

    var CheckCallArgs = "AsmJSValidate.cpp:uint8 CheckCallArgs(FunctionCompiler*, js::frontend::ParseNode*, (uint8)(FunctionCompiler*,js::frontend::ParseNode*,Type)*, FunctionCompiler::Call*)";
    if (name == "checkArg" && caller == CheckCallArgs)
        return true;

    // hook called during script finalization which cannot GC.
    if (/CallDestroyScriptHook/.test(caller))
        return true;

    // template method called during marking and hence cannot GC
    if (name == "op" && caller.indexOf("bool js::WeakMap<Key, Value, HashPolicy>::keyNeedsMark(JSObject*)") != -1)
    {
        return true;
    }

    return false;
}

// Ignore calls through functions pointers with these types
var ignoreClasses = {
    "JSTracer" : true,
    "JSStringFinalizer" : true,
    "SprintfState" : true,
    "SprintfStateStr" : true,
    "JSLocaleCallbacks" : true,
    "JSC::ExecutableAllocator" : true,
    "PRIOMethods": true,
    "XPCOMFunctions" : true, // I'm a little unsure of this one
    "_MD_IOVector" : true,
    "malloc_table_t": true, // replace_malloc
};

// Ignore calls through TYPE.FIELD, where TYPE is the class or struct name containing
// a function pointer field named FIELD.
var ignoreCallees = {
    "js::Class.trace" : true,
    "js::Class.finalize" : true,
    "JSRuntime.destroyPrincipals" : true,
    "icu_50::UObject.__deleting_dtor" : true, // destructors in ICU code can't cause GC
    "mozilla::CycleCollectedJSRuntime.DescribeCustomObjects" : true, // During tracing, cannot GC.
    "mozilla::CycleCollectedJSRuntime.NoteCustomGCThingXPCOMChildren" : true, // During tracing, cannot GC.
    "PLDHashTableOps.hashKey" : true,
    "z_stream_s.zfree" : true,
    "GrGLInterface.fCallback" : true,
};

function fieldCallCannotGC(csu, fullfield)
{
    if (csu in ignoreClasses)
        return true;
    if (fullfield in ignoreCallees)
        return true;
    return false;
}

function ignoreEdgeUse(edge, variable)
{
    // Functions which should not be treated as using variable.
    if (edge.Kind == "Call") {
        var callee = edge.Exp[0];
        if (callee.Kind == "Var") {
            var name = callee.Variable.Name[0];
            if (/~DebugOnly/.test(name))
                return true;
            if (/~ScopedThreadSafeStringInspector/.test(name))
                return true;
        }
    }

    return false;
}

function ignoreEdgeAddressTaken(edge)
{
    // Functions which may take indirect pointers to unrooted GC things,
    // but will copy them into rooted locations before calling anything
    // that can GC. These parameters should usually be replaced with
    // handles or mutable handles.
    if (edge.Kind == "Call") {
        var callee = edge.Exp[0];
        if (callee.Kind == "Var") {
            var name = callee.Variable.Name[0];
            if (/js::Invoke\(/.test(name))
                return true;
        }
    }

    return false;
}

// Ignore calls of these functions (so ignore any stack containing these)
var ignoreFunctions = {
    "ptio.c:pt_MapError" : true,
    "je_malloc_printf" : true,
    "PR_ExplodeTime" : true,
    "PR_ErrorInstallTable" : true,
    "PR_SetThreadPrivate" : true,
    "JSObject* js::GetWeakmapKeyDelegate(JSObject*)" : true, // FIXME: mark with AutoSuppressGCAnalysis instead
    "uint8 NS_IsMainThread()" : true,

    // Bug 1056410 - devirtualization prevents the standard nsISupports::Release heuristic from working
    "uint32 nsXPConnect::Release()" : true,

    // FIXME!
    "NS_LogInit": true,
    "NS_LogTerm": true,
    "NS_LogAddRef": true,
    "NS_LogRelease": true,
    "NS_LogCtor": true,
    "NS_LogDtor": true,
    "NS_LogCOMPtrAddRef": true,
    "NS_LogCOMPtrRelease": true,

    // FIXME!
    "NS_DebugBreak": true,

    // These are a little overzealous -- these destructors *can* GC if they end
    // up wrapping a pending exception. See bug 898815 for the heavyweight fix.
    "void js::AutoCompartment::~AutoCompartment(int32)" : true,
    "void JSAutoCompartment::~JSAutoCompartment(int32)" : true,

    // Bug 948646 - the only thing AutoJSContext's constructor calls
    // is an Init() routine whose entire body is covered with an
    // AutoSuppressGCAnalysis. AutoSafeJSContext is the same thing, just with
    // a different value for the 'aSafe' parameter.
    "void mozilla::AutoJSContext::AutoJSContext(mozilla::detail::GuardObjectNotifier*)" : true,
    "void mozilla::AutoSafeJSContext::~AutoSafeJSContext(int32)" : true,

    // And these are workarounds to avoid even more analysis work,
    // which would sadly still be needed even with bug 898815.
    "void js::AutoCompartment::AutoCompartment(js::ExclusiveContext*, JSCompartment*)": true,
};

function ignoreGCFunction(mangled)
{
    assert(mangled in readableNames);
    var fun = readableNames[mangled][0];

    if (fun in ignoreFunctions)
        return true;

    // Templatized function
    if (fun.indexOf("void nsCOMPtr<T>::Assert_NoQueryNeeded()") >= 0)
        return true;

    // XXX modify refillFreeList<NoGC> to not need data flow analysis to understand it cannot GC.
    if (/refillFreeList/.test(fun) && /\(js::AllowGC\)0u/.test(fun))
        return true;
    return false;
}

function isRootedTypeName(name)
{
    if (name == "mozilla::ErrorResult" ||
        name == "JSErrorResult" ||
        name == "WrappableJSErrorResult" ||
        name == "js::frontend::TokenStream" ||
        name == "js::frontend::TokenStream::Position" ||
        name == "ModuleCompiler" ||
        name == "JSAddonId")
    {
        return true;
    }
    return false;
}

function stripUCSAndNamespace(name)
{
    if (name.startsWith('struct '))
        name = name.substr(7);
    if (name.startsWith('class '))
        name = name.substr(6);
    if (name.startsWith('const '))
        name = name.substr(6);
    if (name.startsWith('js::ctypes::'))
        name = name.substr(12);
    if (name.startsWith('js::'))
        name = name.substr(4);
    if (name.startsWith('JS::'))
        name = name.substr(4);
    if (name.startsWith('mozilla::dom::'))
        name = name.substr(14);
    if (name.startsWith('mozilla::'))
        name = name.substr(9);

    return name;
}

function isRootedPointerTypeName(name)
{
    name = stripUCSAndNamespace(name);

    if (name.startsWith('MaybeRooted<'))
        return /\(js::AllowGC\)1u>::RootType/.test(name);

    return name.startsWith('Rooted') || name.startsWith('PersistentRooted');
}

function isUnsafeStorage(typeName)
{
    typeName = stripUCSAndNamespace(typeName);
    return typeName.startsWith('UniquePtr<');
}

function isSuppressConstructor(name)
{
    return name.indexOf("::AutoSuppressGC") != -1
        || name.indexOf("::AutoAssertGCCallback") != -1
        || name.indexOf("::AutoEnterAnalysis") != -1
        || name.indexOf("::AutoSuppressGCAnalysis") != -1
        || name.indexOf("::AutoIgnoreRootingHazards") != -1;
}

// nsISupports subclasses' methods may be scriptable (or overridden
// via binary XPCOM), and so may GC. But some fields just aren't going
// to get overridden with something that can GC.
function isOverridableField(initialCSU, csu, field)
{
    if (csu != 'nsISupports')
        return false;
    if (field == 'GetCurrentJSContext')
        return false;
    if (field == 'IsOnCurrentThread')
        return false;
    if (field == 'GetNativeContext')
        return false;
    if (field == "GetGlobalJSObject")
        return false;
    if (field == "GetIsMainThread")
        return false;
    if (initialCSU == 'nsIXPConnectJSObjectHolder' && field == 'GetJSObject')
        return false;
    if (initialCSU == 'nsIXPConnect' && field == 'GetSafeJSContext')
        return false;
    if (initialCSU == 'nsIScriptContext') {
        if (field == 'GetWindowProxy' || field == 'GetWindowProxyPreserveColor')
            return false;
    }

    return true;
}
