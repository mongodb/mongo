/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

"use strict";

loadRelativeToScript('callgraph.js');

var theFunctionNameToFind;
if (scriptArgs[0] == '--function') {
    theFunctionNameToFind = scriptArgs[1];
    scriptArgs = scriptArgs.slice(2);
}

var typeInfo_filename = scriptArgs[0] || "typeInfo.txt";

var memoized = new Map();
var memoizedCount = 0;

function memo(name)
{
    if (!memoized.has(name)) {
        let id = memoized.size + 1;
        memoized.set(name, "" + id);
        print(`#${id} ${name}`);
    }
    return memoized.get(name);
}

var lastline;
function printOnce(line)
{
    if (line != lastline) {
        print(line);
        lastline = line;
    }
}

// Returns a table mapping function name to lists of [annotation-name,
// annotation-value] pairs: { function-name => [ [annotation-name, annotation-value] ] }
function getAnnotations(body)
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

function getTags(functionName, body) {
    var tags = new Set();
    var annotations = getAnnotations(body);
    if (functionName in annotations) {
        for (var [ annName, annValue ] of annotations[functionName]) {
            if (annName == 'Tag')
                tags.add(annValue);
        }
    }
    return tags;
}

function processBody(functionName, body)
{
    if (!('PEdge' in body))
        return;

    for (var tag of getTags(functionName, body).values())
        print("T " + memo(functionName) + " " + tag);

    // Set of all callees that have been output so far, in order to suppress
    // repeated callgraph edges from being recorded. Use a separate set for
    // suppressed callees, since we don't want a suppressed edge (within one
    // RAII scope) to prevent an unsuppressed edge from being recorded. The
    // seen array is indexed by a boolean 'suppressed' variable.
    var seen = [ new Set(), new Set() ];

    lastline = null;
    for (var edge of body.PEdge) {
        if (edge.Kind != "Call")
            continue;

        // Whether this call is within the RAII scope of a GC suppression class
        var edgeSuppressed = (edge.Index[0] in body.suppressed);

        for (var callee of getCallees(edge)) {
            var suppressed = Boolean(edgeSuppressed || callee.suppressed);
            var prologue = suppressed ? "SUPPRESS_GC " : "";
            prologue += memo(functionName) + " ";
            if (callee.kind == 'direct') {
                if (!seen[+suppressed].has(callee.name)) {
                    seen[+suppressed].add(callee.name);
                    printOnce("D " + prologue + memo(callee.name));
                }
            } else if (callee.kind == 'field') {
                var { csu, field, isVirtual } = callee;
                const tag = isVirtual ? 'V' : 'F';
                printOnce(tag + " " + prologue + "CLASS " + csu + " FIELD " + field);
            } else if (callee.kind == 'resolved-field') {
                // Fully-resolved field (virtual method) call. Record the
                // callgraph edges. Do not consider suppression, since it is
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
                        printOnce("R " + memo(fullFieldName) + " " + memo(target.name));
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
        body.suppressed = [];

    for (var body of functionBodies) {
        for (var [pbody, id] of allRAIIGuardedCallPoints(typeInfo, functionBodies, body, isSuppressConstructor))
            pbody.suppressed[id] = true;
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
        print("D " + memo(inChargeXTor) + " " + memo(functionName));

        // Bug 1056410: Oh joy. GCC does something even funkier internally,
        // where it generates calls to ~Foo() but a body for ~Foo(int32) even
        // though it uses the same mangled name for both. So we need to add a
        // synthetic edge from ~Foo() -> ~Foo(int32).
        //
        // inChargeXTor will have the (int32).
        if (functionName.indexOf("::~") > 0) {
            var calledDestructor = inChargeXTor.replace("(int32)", "()");
            print("D " + memo(calledDestructor) + " " + memo(inChargeXTor));
        }
    }

    // Further note: from http://mentorembedded.github.io/cxx-abi/abi.html the
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
    if (functionName.indexOf("C4E") != -1 || functionName.indexOf("D4Ev") != -1) {
        var [ mangled, unmangled ] = splitFunction(functionName);
        // E terminates the method name (and precedes the method parameters).
        // If eg "C4E" shows up in the mangled name for another reason, this
        // will create bogus edges in the callgraph. But will affect little and
        // is somewhat difficult to avoid, so we will live with it.
        for (let [synthetic, variant] of [['C4E', 'C1E'],
                                          ['C4E', 'C2E'],
                                          ['C4E', 'C3E'],
                                          ['D4Ev', 'D1Ev'],
                                          ['D4Ev', 'D2Ev'],
                                          ['D4Ev', 'D3Ev']])
        {
            if (mangled.indexOf(synthetic) == -1)
                continue;

            let variant_mangled = mangled.replace(synthetic, variant);
            let variant_full = variant_mangled + "$" + unmangled;
            print("D " + memo(variant_full) + " " + memo(functionName));
        }
    }
}

for (var nameIndex = minStream; nameIndex <= maxStream; nameIndex++) {
    var name = xdb.read_key(nameIndex);
    var data = xdb.read_entry(name);
    process(name.readString(), JSON.parse(data.readString()));
    xdb.free_string(name);
    xdb.free_string(data);
}
