/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

"use strict";

// Ignore calls made through these function pointers
var ignoreIndirectCalls = {
    "mallocSizeOf" : true,
    "aMallocSizeOf" : true,
    "__conv" : true,
    "__convf" : true,
    "prerrortable.c:callback_newtable" : true,
    "mozalloc_oom.cpp:void (* gAbortHandler)(size_t)" : true,
};

// Types that when constructed with no arguments, are "safe" values (they do
// not contain GC pointers).
var typesWithSafeConstructors = new Set([
    "mozilla::Maybe",
    "mozilla::dom::Nullable",
    "mozilla::dom::Optional",
    "mozilla::UniquePtr",
    "js::UniquePtr"
]);

var resetterMethods = {
    'mozilla::Maybe': new Set(["reset"]),
    'mozilla::UniquePtr': new Set(["reset"]),
    'js::UniquePtr': new Set(["reset"]),
    'mozilla::dom::Nullable': new Set(["SetNull"]),
    'mozilla::dom::TypedArray_base': new Set(["Reset"]),
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

    // hook called during script finalization which cannot GC.
    if (/CallDestroyScriptHook/.test(caller))
        return true;

    // Call through a 'callback' function pointer, in a place where we're going
    // to be throwing a JS exception.
    if (name == "callback" && caller.includes("js::ErrorToException"))
        return true;

    // The math cache only gets called with non-GC math functions.
    if (name == "f" && caller.includes("js::MathCache::lookup"))
        return true;

    // It would probably be better to somehow rewrite PR_CallOnce(foo) into a
    // call of foo, but for now just assume that nobody is crazy enough to use
    // PR_CallOnce with a function that can GC.
    if (name == "func" && caller == "PR_CallOnce")
        return true;

    return false;
}

// Ignore calls through functions pointers with these types
var ignoreClasses = {
    "JSStringFinalizer" : true,
    "SprintfState" : true,
    "SprintfStateStr" : true,
    "JSLocaleCallbacks" : true,
    "JSC::ExecutableAllocator" : true,
    "PRIOMethods": true,
    "_MD_IOVector" : true,
    "malloc_table_t": true, // replace_malloc
    "malloc_hook_table_t": true, // replace_malloc
    "mozilla::MallocSizeOf": true,
    "MozMallocSizeOf": true,
};

// Ignore calls through TYPE.FIELD, where TYPE is the class or struct name containing
// a function pointer field named FIELD.
var ignoreCallees = {
    "js::Class.trace" : true,
    "js::Class.finalize" : true,
    "JSClassOps.trace" : true,
    "JSClassOps.finalize" : true,
    "JSRuntime.destroyPrincipals" : true,
    "icu_50::UObject.__deleting_dtor" : true, // destructors in ICU code can't cause GC
    "mozilla::CycleCollectedJSRuntime.DescribeCustomObjects" : true, // During tracing, cannot GC.
    "mozilla::CycleCollectedJSRuntime.NoteCustomGCThingXPCOMChildren" : true, // During tracing, cannot GC.
    "PLDHashTableOps.hashKey" : true,
    "PLDHashTableOps.clearEntry" : true,
    "z_stream_s.zfree" : true,
    "z_stream_s.zalloc" : true,
    "GrGLInterface.fCallback" : true,
    "std::strstreambuf._M_alloc_fun" : true,
    "std::strstreambuf._M_free_fun" : true,
    "struct js::gc::Callback<void (*)(JSContext*, void*)>.op" : true,
    "mozilla::ThreadSharedFloatArrayBufferList::Storage.mFree" : true,
    "mozilla::SizeOfState.mMallocSizeOf": true,
};

function fieldCallCannotGC(csu, fullfield)
{
    if (csu in ignoreClasses)
        return true;
    if (fullfield in ignoreCallees)
        return true;
    return false;
}

function ignoreEdgeUse(edge, variable, body)
{
    // Horrible special case for ignoring a false positive in xptcstubs: there
    // is a local variable 'paramBuffer' holding an array of nsXPTCMiniVariant
    // on the stack, which appears to be live across a GC call because its
    // constructor is called when the array is initialized, even though the
    // constructor is a no-op. So we'll do a very narrow exclusion for the use
    // that incorrectly started the live range, which was basically "__temp_1 =
    // paramBuffer".
    //
    // By scoping it so narrowly, we can detect most hazards that would be
    // caused by modifications in the PrepareAndDispatch code. It just barely
    // avoids having a hazard already.
    if (('Name' in variable) && (variable.Name[0] == 'paramBuffer')) {
        if (body.BlockId.Kind == 'Function' && body.BlockId.Variable.Name[0] == 'PrepareAndDispatch')
            if (edge.Kind == 'Assign' && edge.Type.Kind == 'Pointer')
                if (edge.Exp[0].Kind == 'Var' && edge.Exp[1].Kind == 'Var')
                    if (edge.Exp[1].Variable.Kind == 'Local' && edge.Exp[1].Variable.Name[0] == 'paramBuffer')
                        return true;
    }

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

// Return whether csu.method is one that we claim can never GC.
function isSuppressedVirtualMethod(csu, method)
{
    return csu == "nsISupports" && (method == "AddRef" || method == "Release");
}

// Ignore calls of these functions (so ignore any stack containing these)
var ignoreFunctions = {
    "ptio.c:pt_MapError" : true,
    "je_malloc_printf" : true,
    "malloc_usable_size" : true,
    "vprintf_stderr" : true,
    "PR_ExplodeTime" : true,
    "PR_ErrorInstallTable" : true,
    "PR_SetThreadPrivate" : true,
    "uint8 NS_IsMainThread()" : true,

    // Has an indirect call under it by the name "__f", which seemed too
    // generic to ignore by itself.
    "void* std::_Locale_impl::~_Locale_impl(int32)" : true,

    // Bug 1056410 - devirtualization prevents the standard nsISupports::Release heuristic from working
    "uint32 nsXPConnect::Release()" : true,
    "uint32 nsAtom::Release()" : true,

    // Allocation API
    "malloc": true,
    "calloc": true,
    "realloc": true,
    "free": true,

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

    // Similar to heap snapshot mock classes, and GTests below. This posts a
    // synchronous runnable when a GTest fails, and we are pretty sure that the
    // particular runnable it posts can't even GC, but the analysis isn't
    // currently smart enough to determine that. In either case, this is (a)
    // only in GTests, and (b) only when the Gtest has already failed. We have
    // static and dynamic checks for no GC in the non-test code, and in the test
    // code we fall back to only the dynamic checks.
    "void test::RingbufferDumper::OnTestPartResult(testing::TestPartResult*)" : true,

    "float64 JS_GetCurrentEmbedderTime()" : true,

    // This calls any JSObjectMovedOp for the tenured object via an indirect call.
    "JSObject* js::TenuringTracer::moveToTenuredSlow(JSObject*)" : true,

    "void js::Nursery::freeMallocedBuffers()" : true,

    "void js::AutoEnterOOMUnsafeRegion::crash(uint64, int8*)" : true,

    "void mozilla::dom::WorkerPrivate::AssertIsOnWorkerThread() const" : true,

    // It would be cool to somehow annotate that nsTHashtable<T> will use
    // nsTHashtable<T>::s_MatchEntry for its matchEntry function pointer, but
    // there is no mechanism for that. So we will just annotate a particularly
    // troublesome logging-related usage.
    "EntryType* nsTHashtable<EntryType>::PutEntry(nsTHashtable<EntryType>::KeyType, const fallible_t&) [with EntryType = nsBaseHashtableET<nsCharPtrHashKey, nsAutoPtr<mozilla::LogModule> >; nsTHashtable<EntryType>::KeyType = const char*; nsTHashtable<EntryType>::fallible_t = mozilla::fallible_t]" : true,
    "EntryType* nsTHashtable<EntryType>::GetEntry(nsTHashtable<EntryType>::KeyType) const [with EntryType = nsBaseHashtableET<nsCharPtrHashKey, nsAutoPtr<mozilla::LogModule> >; nsTHashtable<EntryType>::KeyType = const char*]" : true,
    "EntryType* nsTHashtable<EntryType>::PutEntry(nsTHashtable<EntryType>::KeyType) [with EntryType = nsBaseHashtableET<nsPtrHashKey<const mozilla::BlockingResourceBase>, nsAutoPtr<mozilla::DeadlockDetector<mozilla::BlockingResourceBase>::OrderingEntry> >; nsTHashtable<EntryType>::KeyType = const mozilla::BlockingResourceBase*]" : true,
    "EntryType* nsTHashtable<EntryType>::GetEntry(nsTHashtable<EntryType>::KeyType) const [with EntryType = nsBaseHashtableET<nsPtrHashKey<const mozilla::BlockingResourceBase>, nsAutoPtr<mozilla::DeadlockDetector<mozilla::BlockingResourceBase>::OrderingEntry> >; nsTHashtable<EntryType>::KeyType = const mozilla::BlockingResourceBase*]" : true,

    // VTune internals that lazy-load a shared library and make IndirectCalls.
    "iJIT_IsProfilingActive" : true,
    "iJIT_NotifyEvent": true,

    // The big hammers.
    "PR_GetCurrentThread" : true,
    "calloc" : true,

    // This will happen early enough in initialization to not matter.
    "_PR_UnixInit" : true,

    "uint8 nsContentUtils::IsExpandedPrincipal(nsIPrincipal*)" : true,

    "void mozilla::AutoProfilerLabel::~AutoProfilerLabel(int32)" : true,

    // Stores a function pointer in an AutoProfilerLabelData struct and calls it.
    // And it's in mozglue, which doesn't have access to the attributes yet.
    "void mozilla::ProfilerLabelEnd(mozilla::Tuple<void*, unsigned int>*)" : true,

    // This gets into PLDHashTable function pointer territory, and should get
    // set up early enough to not do anything when it matters anyway.
    "mozilla::LogModule* mozilla::LogModule::Get(int8*)": true,

    // This annotation is correct, but the reasoning is still being hashed out
    // in bug 1582326 comment 8 and on.
    "nsCycleCollector.cpp:nsISupports* CanonicalizeXPCOMParticipant(nsISupports*)": true,

    // PLDHashTable again
    "void mozilla::DeadlockDetector<T>::Add(const T*) [with T = mozilla::BlockingResourceBase]": true,

    // OOM handling during logging
    "void mozilla::detail::log_print(mozilla::LogModule*, int32, int8*)": true,

    // This would need to know that the nsCOMPtr refcount will not go to zero.
    "uint8 XPCJSRuntime::DescribeCustomObjects(JSObject*, JSClass*, int8[72]*)[72]) const": true,

    // As the comment says "Refcount isn't zero, so Suspect won't delete anything."
    "uint64 nsCycleCollectingAutoRefCnt::incr(void*, nsCycleCollectionParticipant*) [with void (* suspect)(void*, nsCycleCollectionParticipant*, nsCycleCollectingAutoRefCnt*, bool*) = NS_CycleCollectorSuspect3; uintptr_t = long unsigned int]": true,

    // Calls MergeSort
    "uint8 v8::internal::RegExpDisjunction::SortConsecutiveAtoms(v8::internal::RegExpCompiler*)": true,

    // nsIEventTarget.IsOnCurrentThreadInfallible does not get resolved, and
    // this is called on non-JS threads so cannot use AutoSuppressGCAnalysis.
    "uint8 nsAutoOwningEventTarget::IsCurrentThread() const": true,
};

function extraGCFunctions() {
    return ["ffi_call"].filter(f => f in readableNames);
}

function isProtobuf(name)
{
    return name.match(/\bgoogle::protobuf\b/) ||
           name.match(/\bmozilla::devtools::protobuf\b/);
}

function isHeapSnapshotMockClass(name)
{
    return name.match(/\bMockWriter\b/) ||
           name.match(/\bMockDeserializedNode\b/);
}

function isGTest(name)
{
    return name.match(/\btesting::/);
}

function isICU(name)
{
    return name.match(/\bicu_\d+::/) ||
           name.match(/u(prv_malloc|prv_realloc|prv_free|case_toFullLower)_\d+/)
}

function ignoreGCFunction(mangled)
{
    // Field calls will not be in readableNames
    if (!(mangled in readableNames))
        return false;

    const fun = readableNames[mangled][0];

    if (fun in ignoreFunctions)
        return true;

    // The protobuf library, and [de]serialization code generated by the
    // protobuf compiler, uses a _ton_ of function pointers but they are all
    // internal. The same is true for ICU. Easiest to just ignore that mess
    // here.
    if (isProtobuf(fun) || isICU(fun))
        return true;

    // Ignore anything that goes through heap snapshot GTests or mocked classes
    // used in heap snapshot GTests. GTest and GMock expose a lot of virtual
    // methods and function pointers that could potentially GC after an
    // assertion has already failed (depending on user-provided code), but don't
    // exhibit that behavior currently. For non-test code, we have dynamic and
    // static checks that ensure we don't GC. However, for test code we opt out
    // of static checks here, because of the above stated GMock/GTest issues,
    // and rely on only the dynamic checks provided by AutoAssertCannotGC.
    if (isHeapSnapshotMockClass(fun) || isGTest(fun))
        return true;

    // Templatized function
    if (fun.includes("void nsCOMPtr<T>::Assert_NoQueryNeeded()"))
        return true;

    // Bug 1577915 - Sixgill is ignoring a template param that makes its CFG
    // impossible.
    if (fun.includes("UnwrapObjectInternal") && fun.includes("mayBeWrapper = false"))
        return true;

    // These call through an 'op' function pointer.
    if (fun.includes("js::WeakMap<Key, Value, HashPolicy>::getDelegate("))
        return true;

    // TODO: modify refillFreeList<NoGC> to not need data flow analysis to
    // understand it cannot GC. As of gcc 6, the same problem occurs with
    // tryNewTenuredThing, tryNewNurseryObject, and others.
    if (/refillFreeList|tryNew/.test(fun) && /= js::NoGC/.test(fun))
        return true;

    return false;
}

function stripUCSAndNamespace(name)
{
    name = name.replace(/(struct|class|union|const) /g, "");
    name = name.replace(/(js::ctypes::|js::|JS::|mozilla::dom::|mozilla::)/g, "");
    return name;
}

function extraRootedGCThings()
{
    return [ 'JSAddonId' ];
}

function extraRootedPointers()
{
    return [
    ];
}

function isRootedGCPointerTypeName(name)
{
    name = stripUCSAndNamespace(name);

    if (name.startsWith('MaybeRooted<'))
        return /\(js::AllowGC\)1u>::RootType/.test(name);

    return false;
}

function isUnsafeStorage(typeName)
{
    typeName = stripUCSAndNamespace(typeName);
    return typeName.startsWith('UniquePtr<');
}

// If edgeType is a constructor type, return whatever limits it implies for its
// scope (or zero if not matching).
function isLimitConstructor(typeInfo, edgeType, varName)
{
    // Check whether this could be a constructor
    if (edgeType.Kind != 'Function')
        return 0;
    if (!('TypeFunctionCSU' in edgeType))
        return 0;
    if (edgeType.Type.Kind != 'Void')
        return 0;

    // Check whether the type is a known suppression type.
    var type = edgeType.TypeFunctionCSU.Type.Name;
    let limit = 0;
    if (type in typeInfo.GCSuppressors)
        limit = limit | LIMIT_CANNOT_GC;

    // And now make sure this is the constructor, not some other method on a
    // suppression type. varName[0] contains the qualified name.
    var [ mangled, unmangled ] = splitFunction(varName[0]);
    if (mangled.search(/C\d[EI]/) == -1)
        return 0; // Mangled names of constructors have C<num>E or C<num>I
    var m = unmangled.match(/([~\w]+)(?:<.*>)?\(/);
    if (!m)
        return 0;
    var type_stem = type.replace(/\w+::/g, '').replace(/\<.*\>/g, '');
    if (m[1] != type_stem)
        return 0;

    return limit;
}

// nsISupports subclasses' methods may be scriptable (or overridden
// via binary XPCOM), and so may GC. But some fields just aren't going
// to get overridden with something that can GC.
function isOverridableField(staticCSU, csu, field)
{
    if (csu != 'nsISupports')
        return false;

    // Now that binary XPCOM is dead, all these annotations should be replaced
    // with something based on bug 1347999.
    if (field == 'GetCurrentJSContext')
        return false;
    if (field == 'IsOnCurrentThread')
        return false;
    if (field == 'GetNativeContext')
        return false;
    if (field == "GetGlobalJSObject")
        return false;
    if (field == "GetGlobalJSObjectPreserveColor")
        return false;
    if (field == "GetIsMainThread")
        return false;
    if (field == "GetThreadFromPRThread")
        return false;
    if (field == "DocAddSizeOfIncludingThis")
        return false;
    if (field == "ConstructUbiNode")
        return false;

    // Fields on the [builtinclass] nsIPrincipal
    if (field == "GetSiteOrigin")
        return false;
    if (field == "GetDomain")
        return false;
    if (field == "GetBaseDomain")
        return false;
    if (field == "GetOriginNoSuffix")
        return false;

    // Fields on nsIURI
    if (field == "GetScheme")
        return false;
    if (field == "GetAsciiHostPort")
        return false;
    if (field == "GetAsciiSpec")
        return false;
    if (field == "SchemeIs")
        return false;

    if (staticCSU == 'nsIXPCScriptable' && field == "GetScriptableFlags")
        return false;
    if (staticCSU == 'nsIXPConnectJSObjectHolder' && field == 'GetJSObject')
        return false;
    if (staticCSU == 'nsIXPConnect' && field == 'GetSafeJSContext')
        return false;

    // nsIScriptSecurityManager is not [builtinclass], but smaug says "the
    // interface definitely should be builtinclass", which is good enough.
    if (staticCSU == 'nsIScriptSecurityManager' && field == 'IsSystemPrincipal')
        return false;

    if (staticCSU == 'nsIScriptContext') {
        if (field == 'GetWindowProxy' || field == 'GetWindowProxyPreserveColor')
            return false;
    }
    return true;
}

function listNonGCPointers() {
    return [
        // Safe only because jsids are currently only made from pinned strings.
        'NPIdentifier',
    ];
}

function isJSNative(mangled)
{
    // _Z...E = function
    // 9JSContext = JSContext*
    // j = uint32
    // PN2JS5Value = JS::Value*
    //   P = pointer
    //   N2JS = JS::
    //   5Value = Value
    return mangled.endsWith("P9JSContextjPN2JS5ValueE") && mangled.startsWith("_Z");
}
