/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

"use strict";

loadRelativeToScript('utility.js');

// Functions come out of sixgill in the form "mangled|readable". The mangled
// name is Truth. One mangled name might correspond to multiple readable names,
// for multiple reasons, including (1) sixgill/gcc doesn't always qualify types
// the same way or de-typedef the same amount; (2) sixgill's output treats
// references and pointers the same, and so doesn't distinguish them, but C++
// treats them as separate for overloading and linking; (3) (identical)
// destructors sometimes have an int32 parameter, sometimes not.
//
// The readable names are useful because they're far more meaningful to the
// user, and are what should show up in reports and questions to mrgiggles. At
// least in most cases, it's fine to have the extra mangled name tacked onto
// the beginning for these.
//
// The strategy used is to separate out the pieces whenever they are read in,
// create a table mapping mangled names to (one of the) readable names, and
// use the mangled names in all computation.
//
// Note that callgraph.txt uses a compressed representation -- each name is
// mapped to an integer, and those integers are what is recorded in the edges.
// But the integers depend on the full name, whereas the true edge should only
// consider the mangled name. And some of the names encoded in callgraph.txt
// are FieldCalls, not just function names.

var readableNames = {}; // map from mangled name => list of readable names
var mangledName = {}; // map from demangled names => mangled names. Could be eliminated.
var calleeGraph = {}; // map from mangled => list of tuples of {'callee':mangled, 'suppressed':bool}
var callerGraph = {}; // map from mangled => list of tuples of {'caller':mangled, 'suppressed':bool}
var gcFunctions = {}; // map from mangled callee => reason
var suppressedFunctions = {}; // set of mangled names (map from mangled name => true)
var gcEdges = {};

function addGCFunction(caller, reason)
{
    if (caller in suppressedFunctions)
        return false;

    if (ignoreGCFunction(caller))
        return false;

    if (!(caller in gcFunctions)) {
        gcFunctions[caller] = reason;
        return true;
    }

    return false;
}

function addCallEdge(caller, callee, suppressed)
{
    addToKeyedList(calleeGraph, caller, {callee:callee, suppressed:suppressed});
    addToKeyedList(callerGraph, callee, {caller:caller, suppressed:suppressed});
}

// Map from identifier to full "mangled|readable" name. Or sometimes to a
// Class.Field name.
var functionNames = [""];

// Map from identifier to mangled name (or to a Class.Field)
var idToMangled = [""];

function loadCallgraph(file)
{
    var suppressedFieldCalls = {};
    var resolvedFunctions = {};

    var numGCCalls = 0;

    for (var line of readFileLines_gen(file)) {
        line = line.replace(/\n/, "");

        var match;
        if (match = line.charAt(0) == "#" && /^\#(\d+) (.*)/.exec(line)) {
            assert(functionNames.length == match[1]);
            functionNames.push(match[2]);
            var [ mangled, readable ] = splitFunction(match[2]);
            if (mangled in readableNames)
                readableNames[mangled].push(readable);
            else
                readableNames[mangled] = [ readable ];
            mangledName[readable] = mangled;
            idToMangled.push(mangled);
            continue;
        }
        var suppressed = false;
        if (line.indexOf("SUPPRESS_GC") != -1) {
            match = /^(..)SUPPRESS_GC (.*)/.exec(line);
            line = match[1] + match[2];
            suppressed = true;
        }
        var tag = line.charAt(0);
        if (match = tag == 'I' && /^I (\d+) VARIABLE ([^\,]*)/.exec(line)) {
            var mangledCaller = idToMangled[match[1]];
            var name = match[2];
            if (!indirectCallCannotGC(functionNames[match[1]], name) && !suppressed)
                addGCFunction(mangledCaller, "IndirectCall: " + name);
        } else if (match = (tag == 'F' || tag == 'V') && /^[FV] (\d+) CLASS (.*?) FIELD (.*)/.exec(line)) {
            var caller = idToMangled[match[1]];
            var csu = match[2];
            var fullfield = csu + "." + match[3];
            if (suppressed)
                suppressedFieldCalls[fullfield] = true;
            else if (!fieldCallCannotGC(csu, fullfield))
                addGCFunction(caller, "FieldCall: " + fullfield);
        } else if (match = tag == 'D' && /^D (\d+) (\d+)/.exec(line)) {
            var caller = idToMangled[match[1]];
            var callee = idToMangled[match[2]];
            addCallEdge(caller, callee, suppressed);
        } else if (match = tag == 'R' && /^R (\d+) (\d+)/.exec(line)) {
            var callerField = idToMangled[match[1]];
            var callee = idToMangled[match[2]];
            addCallEdge(callerField, callee, false);
            resolvedFunctions[callerField] = true;
        } else if (match = tag == 'T' && /^T (\d+) (.*)/.exec(line)) {
            var mangled = idToMangled[match[1]];
            var tag = match[2];
            if (tag == 'GC Call') {
                addGCFunction(mangled, "GC");
                numGCCalls++;
            }
        }
    }

    // Add in any extra functions at the end. (If we did this early, it would
    // mess up the id <-> name correspondence. Also, we need to know if the
    // functions even exist in the first place.)
    for (var func of extraGCFunctions()) {
        addGCFunction(func, "annotation");
    }

    // Initialize suppressedFunctions to the set of all functions, and the
    // worklist to all toplevel callers.
    var worklist = [];
    for (var callee in callerGraph)
        suppressedFunctions[callee] = true;
    for (var caller in calleeGraph) {
        if (!(caller in callerGraph)) {
            suppressedFunctions[caller] = true;
            worklist.push(caller);
        }
    }

    // Find all functions reachable via an unsuppressed call chain, and remove
    // them from the suppressedFunctions set. Everything remaining is only
    // reachable when GC is suppressed.
    var top = worklist.length;
    while (top > 0) {
        name = worklist[--top];
        if (!(name in suppressedFunctions))
            continue;
        delete suppressedFunctions[name];
        if (!(name in calleeGraph))
            continue;
        for (var entry of calleeGraph[name]) {
            if (!entry.suppressed)
                worklist[top++] = entry.callee;
        }
    }

    // Such functions are known to not GC.
    for (var name in gcFunctions) {
        if (name in suppressedFunctions)
            delete gcFunctions[name];
    }

    for (var name in suppressedFieldCalls) {
        suppressedFunctions[name] = true;
    }

    // Sanity check to make sure the callgraph has some functions annotated as
    // GC Calls. This is mostly a check to be sure the earlier processing
    // succeeded (as opposed to, say, running on empty xdb files because you
    // didn't actually compile anything interesting.)
    assert(numGCCalls > 0, "No GC functions found!");

    // Initialize the worklist to all known gcFunctions.
    var worklist = [];
    for (var name in gcFunctions)
        worklist.push(name);

    // Recursively find all callers and add them to the set of gcFunctions.
    while (worklist.length) {
        name = worklist.shift();
        assert(name in gcFunctions);
        if (!(name in callerGraph))
            continue;
        for (var entry of callerGraph[name]) {
            if (!entry.suppressed && addGCFunction(entry.caller, name))
                worklist.push(entry.caller);
        }
    }

    // Any field call that has been resolved to all possible callees can be
    // trusted to not GC if all of those callees are known to not GC.
    for (var name in resolvedFunctions) {
        if (!(name in gcFunctions))
            suppressedFunctions[name] = true;
    }
}
