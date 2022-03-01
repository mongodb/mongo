/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

"use strict";

loadRelativeToScript('callgraph.js');

var theFunctionNameToFind;
if (scriptArgs[0] == '--function' || scriptArgs[0] == '-f') {
    theFunctionNameToFind = scriptArgs[1];
    scriptArgs = scriptArgs.slice(2);
}

var typeInfo_filename = scriptArgs[0] || "typeInfo.txt";
var callgraphOut_filename = scriptArgs[1] || "callgraph.txt";

var origOut = os.file.redirect(callgraphOut_filename);

var memoized = new Map();
var memoizedCount = 0;

var JSNativeCaller = Object.create(null);
var JSNatives = [];

var unmangled2id = new Set();

function getId(name)
{
    let id = memoized.get(name);
    if (id !== undefined)
        return id;

    id = memoized.size + 1;
    memoized.set(name, id);
    print(`#${id} ${name}`);

    return id;
}

function functionId(name)
{
    const [mangled, unmangled] = splitFunction(name);
    const id = getId(mangled);

    // Only produce a mangled -> unmangled mapping once, unless there are
    // multiple unmangled names for the same mangled name.
    if (unmangled2id.has(unmangled))
        return id;

    print(`= ${id} ${unmangled}`);
    unmangled2id.add(unmangled);
    return id;
}

var lastline;
function printOnce(line)
{
    if (line != lastline) {
        print(line);
        lastline = line;
    }
}

// Returns a table mapping function name to lists of
// [annotation-name, annotation-value] pairs:
//   { function-name => [ [annotation-name, annotation-value] ] }
//
// Note that sixgill will only store certain attributes (annotation-names), so
// this won't be *all* the attributes in the source, just the ones that sixgill
// watches for.
function getAllAttributes(body)
{
    var all_annotations = {};
    for (var v of (body.DefineVariable || [])) {
        if (v.Variable.Kind != 'Func')
            continue;
        var name = v.Variable.Name[0];
        var annotations = all_annotations[name] = [];

        for (var ann of (v.Type.Annotation || [])) {
            annotations.push(ann.Name);
        }
    }

    return all_annotations;
}

// Get just the annotations understood by the hazard analysis.
function getAnnotations(functionName, body) {
    var tags = new Set();
    var attributes = getAllAttributes(body);
    if (functionName in attributes) {
        for (var [ annName, annValue ] of attributes[functionName]) {
            if (annName == 'annotate')
                tags.add(annValue);
        }
    }
    return tags;
}

// Scan through a function body, pulling out all annotations and calls and
// recording them in callgraph.txt.
function processBody(functionName, body)
{
    if (!('PEdge' in body))
        return;


    for (var tag of getAnnotations(functionName, body).values()) {
        print("T " + functionId(functionName) + " " + tag);
        if (tag == "Calls JSNatives")
            JSNativeCaller[functionName] = true;
    }

    // Set of all callees that have been output so far, in order to suppress
    // repeated callgraph edges from being recorded. This uses a Map from
    // callees to limit sets, because we don't want a limited edge to prevent
    // an unlimited edge from being recorded later. (So an edge will be skipped
    // if it exists and is at least as limited as the previously seen edge.)
    //
    // Limit sets are implemented as integers interpreted as bitfields.
    //
    var seen = new Map();

    lastline = null;
    for (var edge of body.PEdge) {
        if (edge.Kind != "Call")
            continue;

        // The limits (eg LIMIT_CANNOT_GC) are determined by whatever RAII
        // scopes might be active, which have been computed previously for all
        // points in the body.
        var edgeLimited = body.limits[edge.Index[0]] | 0;

        for (var callee of getCallees(edge)) {
            // Individual callees may have additional limits. The only such
            // limit currently is that nsISupports.{AddRef,Release} are assumed
            // to never GC.
            const limits = edgeLimited | callee.limits;
            let prologue = limits ? `/${limits} ` : "";
            prologue += functionId(functionName) + " ";
            if (callee.kind == 'direct') {
                const prev_limits = seen.has(callee.name) ? seen.get(callee.name) : LIMIT_UNVISITED;
                if (prev_limits & ~limits) {
                    // Only output an edge if it loosens a limit.
                    seen.set(callee.name, prev_limits & limits);
                    printOnce("D " + prologue + functionId(callee.name));
                }
            } else if (callee.kind == 'field') {
                var { csu, field, isVirtual } = callee;
                const tag = isVirtual ? 'V' : 'F';
                const fullfield = `${csu}.${field}`;
                printOnce(`${tag} ${prologue}${getId(fullfield)} CLASS ${csu} FIELD ${field}`);
            } else if (callee.kind == 'resolved-field') {
                // Fully-resolved field (virtual method) call. Record the
                // callgraph edges. Do not consider limits, since they are
                // local to this callsite and we are writing out a global
                // record here.
                //
                // Any field call that does *not* have an R entry must be
                // assumed to call anything.
                var { csu, field, callees } = callee;
                var fullFieldName = csu + "." + field;
                if (!virtualResolutionsSeen.has(fullFieldName)) {
                    virtualResolutionsSeen.add(fullFieldName);
                    for (var target of callees)
                        printOnce("R " + getId(fullFieldName) + " " + functionId(target.name));
                }
            } else if (callee.kind == 'indirect') {
                printOnce("I " + prologue + "VARIABLE " + callee.variable);
            } else if (callee.kind == 'unknown') {
                printOnce("I " + prologue + "VARIABLE UNKNOWN");
            } else {
                printErr("invalid " + callee.kind + " callee");
                debugger;
            }
        }
    }
}

var typeInfo = loadTypeInfo(typeInfo_filename);

loadTypes("src_comp.xdb");

var xdb = xdbLibrary();
xdb.open("src_body.xdb");

printErr("Finished loading data structures");

var minStream = xdb.min_data_stream();
var maxStream = xdb.max_data_stream();

if (theFunctionNameToFind) {
    var index = xdb.lookup_key(theFunctionNameToFind);
    if (!index) {
        printErr("Function not found");
        quit(1);
    }
    minStream = maxStream = index;
}

function process(functionName, functionBodies)
{
    for (var body of functionBodies)
        body.limits = [];

    for (var body of functionBodies) {
        for (var [pbody, id, limits] of allRAIIGuardedCallPoints(typeInfo, functionBodies, body, isLimitConstructor)) {
            pbody.limits[id] = limits;
        }
    }

    for (var body of functionBodies)
        processBody(functionName, body);

    // GCC generates multiple constructors and destructors ("in-charge" and
    // "not-in-charge") to handle virtual base classes. They are normally
    // identical, and it appears that GCC does some magic to alias them to the
    // same thing. But this aliasing is not visible to the analysis. So we'll
    // add a dummy call edge from "foo" -> "foo *INTERNAL* ", since only "foo"
    // will show up as called but only "foo *INTERNAL* " will be emitted in the
    // case where the constructors are identical.
    //
    // This is slightly conservative in the case where they are *not*
    // identical, but that should be rare enough that we don't care.
    var markerPos = functionName.indexOf(internalMarker);
    if (markerPos > 0) {
        var inChargeXTor = functionName.replace(internalMarker, "");
        printOnce("D " + functionId(inChargeXTor) + " " + functionId(functionName));
    }

    const [ mangled, unmangled ] = splitFunction(functionName);

    // Further note: from https://itanium-cxx-abi.github.io/cxx-abi/abi.html the
    // different kinds of constructors/destructors are:
    // C1	# complete object constructor
    // C2	# base object constructor
    // C3	# complete object allocating constructor
    // D0	# deleting destructor
    // D1	# complete object destructor
    // D2	# base object destructor
    //
    // In actual practice, I have observed C4 and D4 xtors generated by gcc
    // 4.9.3 (but not 4.7.3). The gcc source code says:
    //
    //   /* This is the old-style "[unified]" constructor.
    //      In some cases, we may emit this function and call
    //      it from the clones in order to share code and save space.  */
    //
    // Unfortunately, that "call... from the clones" does not seem to appear in
    // the CFG we get from GCC. So if we see a C4 constructor or D4 destructor,
    // inject an edge to it from C1, C2, and C3 (or D1, D2, and D3). (Note that
    // C3 isn't even used in current GCC, but add the edge anyway just in
    // case.)
    //
    // from gcc/cp/mangle.c:
    //
    // <special-name> ::= D0 # deleting (in-charge) destructor
    //                ::= D1 # complete object (in-charge) destructor
    //                ::= D2 # base object (not-in-charge) destructor
    // <special-name> ::= C1   # complete object constructor
    //                ::= C2   # base object constructor
    //                ::= C3   # complete object allocating constructor
    //
    // Currently, allocating constructors are never used.
    //
    if (functionName.indexOf("C4") != -1) {
        // E terminates the method name (and precedes the method parameters).
        // If eg "C4E" shows up in the mangled name for another reason, this
        // will create bogus edges in the callgraph. But it will affect little
        // and is somewhat difficult to avoid, so we will live with it.
        //
        // Another possibility! A templatized constructor will contain C4I...E
        // for template arguments.
        //
        for (let [synthetic, variant, desc] of [
            ['C4E', 'C1E', 'complete_ctor'],
            ['C4E', 'C2E', 'base_ctor'],
            ['C4E', 'C3E', 'complete_alloc_ctor'],
            ['C4I', 'C1I', 'complete_ctor'],
            ['C4I', 'C2I', 'base_ctor'],
            ['C4I', 'C3I', 'complete_alloc_ctor']])
        {
            if (mangled.indexOf(synthetic) == -1)
                continue;

            let variant_mangled = mangled.replace(synthetic, variant);
            let variant_full = `${variant_mangled}$${unmangled} [[${desc}]]`;
            printOnce("D " + functionId(variant_full) + " " + functionId(functionName));
        }
    }

    // For destructors:
    //
    // I've never seen D4Ev() + D4Ev(int32), only one or the other. So
    // for a D4Ev of any sort, create:
    //
    //   D0() -> D1()  # deleting destructor calls complete destructor, then deletes
    //   D1() -> D2()  # complete destructor calls base destructor, then destroys virtual bases
    //   D2() -> D4(?) # base destructor might be aliased to unified destructor
    //                 # use whichever one is defined, in-charge or not.
    //                 # ('?') means either () or (int32).
    //
    // Note that this doesn't actually make sense -- D0 and D1 should be
    // in-charge, but gcc doesn't seem to give them the in-charge parameter?!
    //
    if (functionName.indexOf("D4Ev") != -1 && functionName.indexOf("::~") != -1) {
        const not_in_charge_dtor = functionName.replace("(int32)", "()");
        const D0 = not_in_charge_dtor.replace("D4Ev", "D0Ev") + " [[deleting_dtor]]";
        const D1 = not_in_charge_dtor.replace("D4Ev", "D1Ev") + " [[complete_dtor]]";
        const D2 = not_in_charge_dtor.replace("D4Ev", "D2Ev") + " [[base_dtor]]";
        printOnce("D " + functionId(D0) + " " + functionId(D1));
        printOnce("D " + functionId(D1) + " " + functionId(D2));
        printOnce("D " + functionId(D2) + " " + functionId(functionName));
    }

    if (isJSNative(mangled))
        JSNatives.push(functionName);
}

function postprocess_callgraph() {
    for (const caller of Object.keys(JSNativeCaller)) {
        const caller_id = functionId(caller);
        for (const callee of JSNatives)
            printOnce(`D ${caller_id} ${functionId(callee)}`);
    }
}

for (var nameIndex = minStream; nameIndex <= maxStream; nameIndex++) {
    var name = xdb.read_key(nameIndex);
    var data = xdb.read_entry(name);
    process(name.readString(), JSON.parse(data.readString()));
    xdb.free_string(name);
    xdb.free_string(data);
}

postprocess_callgraph();

os.file.close(os.file.redirect(origOut));
