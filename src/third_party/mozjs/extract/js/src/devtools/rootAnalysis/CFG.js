/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

// Utility code for traversing the JSON data structures produced by sixgill.

"use strict";

var TRACING = false;

// When edge.Kind == "Pointer", these are the meanings of the edge.Reference field.
var PTR_POINTER = 0;
var PTR_REFERENCE = 1;
var PTR_RVALUE_REF = 2;

// Find all points (positions within the code) of the body given by the list of
// bodies and the blockId to match (which will specify an outer function or a
// loop within it), recursing into loops if needed.
function findAllPoints(bodies, blockId, bits)
{
    var points = [];
    var body;

    for (var xbody of bodies) {
        if (sameBlockId(xbody.BlockId, blockId)) {
            assert(!body);
            body = xbody;
        }
    }
    assert(body);

    if (!("PEdge" in body))
        return;
    for (var edge of body.PEdge) {
        points.push([body, edge.Index[0], bits]);
        if (edge.Kind == "Loop")
            points.push(...findAllPoints(bodies, edge.BlockId, bits));
    }

    return points;
}

// Visitor of a graph of <body, ppoint> vertexes and sixgill-generated edges,
// where the edges represent the actual computation happening.
//
// Uses the syntax `var Visitor = class { ... }` rather than `class Visitor`
// to allow reloading this file with the JS debugger.
var Visitor = class {
    constructor(bodies) {
        this.visited_bodies = new Map();
        for (const body of bodies) {
            this.visited_bodies.set(body, new Map());
        }
    }

    // Prepend `edge` to the info stored at the successor node, returning
    // the updated info value. This should be overridden by pretty much any
    // subclass, as a traversal's semantics are largely determined by this method.
    extend_path(edge, body, ppoint, successor_value) { return true; }

    // Default implementation does a basic "only visit nodes once" search.
    // (Whether this is BFS/DFS/other is determined by the caller.)

    // Override if you need to revisit nodes. Valid actions are "continue",
    // "prune", and "done". "continue" means continue with the search. "prune"
    // means do not continue to predecessors of this node, only continue with
    // the remaining entries in the work queue. "done" means the
    // whole search is complete even if unvisited nodes remain.
    next_action(prev, current) { return prev ? "prune" : "continue"; }

    // Update the info at a node. If this is the first time the node has been
    // seen, `prev` will be undefined. `current` will be the info computed by
    // `extend_path`. The node will be updated with the return value.
    merge_info(prev, current) { return true; }

    // Default visit() implementation. Subclasses will usually leave this alone
    // and use the other methods as extension points.
    //
    // Take a body, a point within that body, and the info computed by
    // extend_path() for that point when traversing an edge. Return whether the
    // search should continue ("continue"), the search should be pruned and
    // other paths followed ("prune"), or that the whole search is complete and
    // it is time to return a value ("done", and the value returned by
    // merge_info() will be returned by the overall search).
    //
    // Persistently record the value computed so far at each point, and call
    // (overridable) next_action() and merge_info() methods with the previous
    // and freshly-computed value for each point.
    //
    // Often, extend_path() will decide how/whether to continue the search and
    // will return the search action to take, and next_action() will blindly
    // return it if the point has not yet been visited. (And if it has, it will
    // prune this branch of the search so that no point is visited multiple
    // times.)
    visit(body, ppoint, info) {
        const visited_value_table = this.visited_bodies.get(body);
        const existing_value_if_visited = visited_value_table.get(ppoint);
        const action = this.next_action(existing_value_if_visited, info);
        const merged = this.merge_info(existing_value_if_visited, info);
        visited_value_table.set(ppoint, merged);
        return [action, merged];
    }
};

function findMatchingBlock(bodies, blockId) {
    for (const body of bodies) {
        if (sameBlockId(body.BlockId, blockId)) {
            return body;
        }
    }
    assert(false);
}

// For a given function containing a set of bodies, each containing a set of
// ppoints, perform a mostly breadth-first traversal through the complete graph
// of all <body, ppoint> nodes throughout all the bodies of the function.
//
// When traversing, every <body, ppoint> node is associated with a value that
// is assigned or updated whenever it is visited. The overall traversal
// terminates when a given condition is reached, and an arbitrary custom value
// is returned. If the search completes without the termination condition
// being reached, it will return the value associated with the entrypoint
// node, which is initialized to `entrypoint_fallback_value` (and thus serves as
// the fallback return value if all search paths are pruned before reaching
// the entrypoint.)
//
// The traversal is only *mostly* breadth-first because the visitor decides
// whether to stop searching when it sees a node. If a node is visited for a
// second time, the visitor can choose to continue (and thus revisit the node)
// in order to find "better" paths that may include a node more than once.
// The search is done in the "upwards" direction -- as in, it starts at the
// exit point and searches through predecessors.
//
// Override visitor.visit() to return an action and a value. The action
// determines whether the overall search should terminate ('done'), or
// continues looking through the predecessors of the current node ('continue'),
// or whether it should just continue processing the work queue without
// looking at predecessors ('prune').
//
// This allows this function to be used in different ways. If the visitor
// associates a value with each node that chains onto its forward-flow successors
// (predecessors in the "upwards" search order), then a complete path through
// the graph will be returned.
//
// Alternatively, BFS_upwards() can be used to test whether a condition holds
// (eg "the exit point is reachable only after calling SomethingImportant()"),
// in which case no path is needed and the visitor can compute a simple boolean
// every time it encounters a point. Note that `entrypoint_fallback_value` will
// still be returned if the search terminates without ever reaching the
// entrypoint, which is useful for dominator analyses.
//
// See the Visitor base class's implementation of visit(), above, for the
// most commonly used visit logic.
function BFS_upwards(start_body, start_ppoint, bodies, visitor,
                     initial_successor_value = {},
                     entrypoint_fallback_value=null)
{
    let entrypoint_value = entrypoint_fallback_value;

    const work = [[start_body, start_ppoint, null, initial_successor_value]];
    if (TRACING) {
        printErr(`BFS start at ${blockIdentifier(start_body)}:${start_ppoint}`);
    }

    while (work.length > 0) {
        const [body, ppoint, edgeToAdd, successor_value] = work.shift();
        if (TRACING) {
            const s = edgeToAdd ? " : " + str(edgeToAdd) : "";
            printErr(`prepending edge from ${ppoint} to state '${successor_value}'${s}`);
        }
        let value = visitor.extend_path(edgeToAdd, body, ppoint, successor_value);

        const [action,  merged_value] = visitor.visit(body, ppoint, value);
        if (action === "done") {
            return merged_value;
        }
        if (action === "prune") {
            // Do not push anything else to the work queue, but continue processing
            // other branches.
            continue;
        }
        assert(action == "continue");
        value = merged_value;

        const predecessors = getPredecessors(body);
        for (const edge of (predecessors[ppoint] || [])) {
            if (edge.Kind == "Loop") {
                // Propagate the search into the exit point of the loop body.
                const loopBody = findMatchingBlock(bodies, edge.BlockId);
                const loopEnd = loopBody.Index[1];
                work.push([loopBody, loopEnd, null, value]);
                // Don't continue to predecessors here without going through
                // the loop. (The points in this body that enter the loop will
                // be traversed when we reach the entry point of the loop.)
            }
            work.push([body, edge.Index[0], edge, value]);
        }

        // Check for hitting the entry point of a loop body.
        if (ppoint == body.Index[0] && body.BlockId.Kind == "Loop") {
            // Propagate to outer body parents that enter the loop body.
            for (const parent of (body.BlockPPoint || [])) {
                const parentBody = findMatchingBlock(bodies, parent.BlockId);
                work.push([parentBody, parent.Index, null, value]);
            }

            // This point is also preceded by the *end* of this loop, for the
            // previous iteration.
            work.push([body, body.Index[1], null, value]);
        }

        // Check for reaching the entrypoint of the function.
        if (body === start_body && ppoint == body.Index[0]) {
            entrypoint_value = value;
        }
    }

    // The search space was exhausted without finding a 'done' state. That
    // might be because all search paths were pruned before reaching the entry
    // point of the function, in which case entrypoint_value will still be its initial
    // value. (If entrypoint_value has been set, then we may still not have visited the
    // entire graph, if some paths were pruned but at least one made it to the entrypoint.)
    return entrypoint_value;
}

// Given the CFG for the constructor call of some RAII, return whether the
// given edge is the matching destructor call.
function isMatchingDestructor(constructor, edge)
{
    if (edge.Kind != "Call")
        return false;
    var callee = edge.Exp[0];
    if (callee.Kind != "Var")
        return false;
    var variable = callee.Variable;
    assert(variable.Kind == "Func");
    if (variable.Name[1].charAt(0) != '~')
        return false;

    // Note that in some situations, a regular function can begin with '~', so
    // we don't necessarily have a destructor in hand. This is probably a
    // sixgill artifact, but in js::wasm::ModuleGenerator::~ModuleGenerator, a
    // templatized static inline EraseIf is invoked, and it gets named ~EraseIf
    // for some reason.
    if (!("PEdgeCallInstance" in edge))
        return false;

    var constructExp = constructor.PEdgeCallInstance.Exp;
    assert(constructExp.Kind == "Var");

    var destructExp = edge.PEdgeCallInstance.Exp;
    if (destructExp.Kind != "Var")
        return false;

    return sameVariable(constructExp.Variable, destructExp.Variable);
}

// Return all calls within the RAII scope of any constructor matched by
// isConstructor(). (Note that this would be insufficient if you needed to
// treat each instance separately, such as when different regions of a function
// body were guarded by these constructors and you needed to do something
// different with each.)
function allRAIIGuardedCallPoints(typeInfo, bodies, body, isConstructor)
{
    if (!("PEdge" in body))
        return [];

    var points = [];

    for (var edge of body.PEdge) {
        if (edge.Kind != "Call")
            continue;
        var callee = edge.Exp[0];
        if (callee.Kind != "Var")
            continue;
        var variable = callee.Variable;
        assert(variable.Kind == "Func");
        const bits = isConstructor(typeInfo, edge.Type, variable.Name);
        if (!bits)
            continue;
        if (!("PEdgeCallInstance" in edge))
            continue;
        if (edge.PEdgeCallInstance.Exp.Kind != "Var")
            continue;

        points.push(...pointsInRAIIScope(bodies, body, edge, bits));
    }

    return points;
}

// Test whether the given edge is the constructor corresponding to the given
// destructor edge.
function isMatchingConstructor(destructor, edge)
{
    if (edge.Kind != "Call")
        return false;
    var callee = edge.Exp[0];
    if (callee.Kind != "Var")
        return false;
    var variable = callee.Variable;
    if (variable.Kind != "Func")
        return false;
    var name = readable(variable.Name[0]);
    var destructorName = readable(destructor.Exp[0].Variable.Name[0]);
    var match = destructorName.match(/^(.*?::)~(\w+)\(/);
    if (!match) {
        printErr("Unhandled destructor syntax: " + destructorName);
        return false;
    }
    var constructorSubstring = match[1] + match[2];
    if (name.indexOf(constructorSubstring) == -1)
        return false;

    var destructExp = destructor.PEdgeCallInstance.Exp;
    if (destructExp.Kind != "Var")
        return false;

    var constructExp = edge.PEdgeCallInstance.Exp;
    if (constructExp.Kind != "Var")
        return false;

    return sameVariable(constructExp.Variable, destructExp.Variable);
}

function findMatchingConstructor(destructorEdge, body, warnIfNotFound=true)
{
    var worklist = [destructorEdge];
    var predecessors = getPredecessors(body);
    while(worklist.length > 0) {
        var edge = worklist.pop();
        if (isMatchingConstructor(destructorEdge, edge))
            return edge;
        if (edge.Index[0] in predecessors) {
            for (var e of predecessors[edge.Index[0]])
                worklist.push(e);
        }
    }
    if (warnIfNotFound)
        printErr("Could not find matching constructor!");
    return undefined;
}

function pointsInRAIIScope(bodies, body, constructorEdge, bits) {
    var seen = {};
    var worklist = [constructorEdge.Index[1]];
    var points = [];
    while (worklist.length) {
        var point = worklist.pop();
        if (point in seen)
            continue;
        seen[point] = true;
        points.push([body, point, bits]);
        var successors = getSuccessors(body);
        if (!(point in successors))
            continue;
        for (var nedge of successors[point]) {
            if (isMatchingDestructor(constructorEdge, nedge))
                continue;
            if (nedge.Kind == "Loop")
                points.push(...findAllPoints(bodies, nedge.BlockId, bits));
            worklist.push(nedge.Index[1]);
        }
    }

    return points;
}

function isImmobileValue(exp) {
    if (exp.Kind == "Int" && exp.String == "0") {
        return true;
    }
    return false;
}

// Returns whether decl is a body.DefineVariable[] entry for a non-temporary reference.
function isReferenceDecl(decl) {
    return decl.Type.Kind == "Pointer" && decl.Type.Reference != PTR_POINTER && decl.Variable.Kind != "Temp";
}

function expressionIsVariableAddress(exp, variable)
{
    while (exp.Kind == "Fld")
        exp = exp.Exp[0];
    return exp.Kind == "Var" && sameVariable(exp.Variable, variable);
}

function edgeTakesVariableAddress(edge, variable, body)
{
    if (ignoreEdgeUse(edge, variable, body))
        return false;
    if (ignoreEdgeAddressTaken(edge))
        return false;
    switch (edge.Kind) {
    case "Assign":
        return expressionIsVariableAddress(edge.Exp[1], variable);
    case "Call":
        if ("PEdgeCallArguments" in edge) {
            for (var exp of edge.PEdgeCallArguments.Exp) {
                if (expressionIsVariableAddress(exp, variable))
                    return true;
            }
        }
        return false;
    default:
        return false;
    }
}

// Look at an invocation of a virtual method or function pointer contained in a
// field, and return the static type of the invocant (or the containing struct,
// for a function pointer field.)
function getFieldCallInstanceCSU(edge, field)
{
    if ("FieldInstanceFunction" in field) {
        // We have a 'this'.
        const instanceExp = edge.PEdgeCallInstance.Exp;
        if (instanceExp.Kind == 'Drf') {
            // somevar->foo()
            return edge.Type.TypeFunctionCSU.Type.Name;
        } else if (instanceExp.Kind == 'Fld') {
            // somevar.foo()
            return instanceExp.Field.FieldCSU.Type.Name;
        } else if (instanceExp.Kind == 'Index') {
            // A strange construct.
            // C++ code: static_cast<JS::CustomAutoRooter*>(this)->trace(trc);
            // CFG: Call(21,30, this*[-1]{JS::CustomAutoRooter}.trace*(trc*))
            return instanceExp.Type.Name;
        } else if (instanceExp.Kind == 'Var') {
            // C++: reinterpret_cast<SimpleTimeZone*>(gRawGMT)->~SimpleTimeZone();
            // CFG:
            //   # icu_64::SimpleTimeZone::icu_64::SimpleTimeZone.__comp_dtor
            //   [6,7] Call gRawGMT.icu_64::SimpleTimeZone.__comp_dtor ()
            return field.FieldCSU.Type.Name;
        } else {
            printErr("------------------ edge -------------------");
            printErr(JSON.stringify(edge, null, 4));
            printErr("------------------ field -------------------");
            printErr(JSON.stringify(field, null, 4));
            assert(false, `unrecognized FieldInstanceFunction Kind ${instanceExp.Kind}`);
        }
    } else {
        // somefar.foo() where somevar is a field of some CSU.
        return field.FieldCSU.Type.Name;
    }
}

function expressionUsesVariable(exp, variable)
{
    if (exp.Kind == "Var" && sameVariable(exp.Variable, variable))
        return true;
    if (!("Exp" in exp))
        return false;
    for (var childExp of exp.Exp) {
        if (expressionUsesVariable(childExp, variable))
            return true;
    }
    return false;
}

function expressionUsesVariableContents(exp, variable)
{
    if (!("Exp" in exp))
        return false;
    for (var childExp of exp.Exp) {
        if (childExp.Kind == 'Drf') {
            if (expressionUsesVariable(childExp, variable))
                return true;
        } else if (expressionUsesVariableContents(childExp, variable)) {
            return true;
        }
    }
    return false;
}

// Detect simple |return nullptr;| statements.
function isReturningImmobileValue(edge, variable)
{
    if (variable.Kind == "Return") {
        if (edge.Exp[0].Kind == "Var" && sameVariable(edge.Exp[0].Variable, variable)) {
            if (isImmobileValue(edge.Exp[1]))
                return true;
        }
    }
    return false;
}

// If the edge uses the given variable's value, return the earliest point at
// which the use is definite. Usually, that means the source of the edge
// (anything that reaches that source point will end up using the variable, but
// there may be other ways to reach the destination of the edge.)
//
// Return values are implicitly used at the very last point in the function.
// This makes a difference: if an RAII class GCs in its destructor, we need to
// start looking at the final point in the function, not one point back from
// that, since that would skip over the GCing call.
//
// Certain references may be annotated to be live to the end of the function
// as well (eg AutoCheckCannotGC&& parameters).
//
// Note that this returns a nonzero value only if the variable's incoming value is used.
// So this would return 0 for 'obj':
//
//     obj = someFunction();
//
// but these would return a positive value:
//
//     obj = someFunction(obj);
//     obj->foo = someFunction();
//
function edgeUsesVariable(edge, variable, body, liveToEnd=false)
{
    if (ignoreEdgeUse(edge, variable, body))
        return 0;

    if (variable.Kind == "Return") {
        liveToEnd = true;
    }

    if (liveToEnd && body.Index[1] == edge.Index[1] && body.BlockId.Kind == "Function") {
        // The last point in the function body is treated as using the return
        // value. This is the only time the destination point is returned
        // rather than the source point.
        return edge.Index[1];
    }

    var src = edge.Index[0];

    switch (edge.Kind) {

    case "Assign": {
        // Detect `Return := nullptr`.
        if (isReturningImmobileValue(edge, variable))
            return 0;
        const [lhs, rhs] = edge.Exp;
        // Detect `lhs := ...variable...`
        if (expressionUsesVariable(rhs, variable))
            return src;
        // Detect `...variable... := rhs` but not `variable := rhs`. The latter
        // overwrites the previous value of `variable` without using it.
        if (expressionUsesVariable(lhs, variable) && !expressionIsVariable(lhs, variable))
            return src;
        return 0;
    }

    case "Assume":
        return expressionUsesVariableContents(edge.Exp[0], variable) ? src : 0;

    case "Call": {
        const callee = edge.Exp[0];
        if (expressionUsesVariable(callee, variable))
            return src;
        if ("PEdgeCallInstance" in edge) {
            if (expressionUsesVariable(edge.PEdgeCallInstance.Exp, variable)) {
                if (edgeStartsValueLiveRange(edge, variable)) {
                    // If the variable is being constructed, then the incoming
                    // value is not used here; it didn't exist before
                    // construction. (The analysis doesn't get told where
                    // variables are defined, so must infer it from
                    // construction. If the variable does not have a
                    // constructor, its live range may be larger than it really
                    // ought to be if it is defined within a loop body, but
                    // that is conservative.)
                } else {
                    return src;
                }
            }
        }
        if ("PEdgeCallArguments" in edge) {
            for (var exp of edge.PEdgeCallArguments.Exp) {
                if (expressionUsesVariable(exp, variable))
                    return src;
            }
        }
        if (edge.Exp.length == 1)
            return 0;

        // Assigning call result to a variable.
        const lhs = edge.Exp[1];
        if (expressionUsesVariable(lhs, variable) && !expressionIsVariable(lhs, variable))
            return src;
        return 0;
    }

    case "Loop":
        return 0;

    case "Assembly":
        return 0;

    default:
        assert(false);
    }
}

// If `decl` is the body.DefineVariable[] declaration of a reference type, then
// return the expression without the outer dereference. Otherwise, return the
// original expression.
function maybeDereference(exp, decl) {
    if (exp.Kind == "Drf" && exp.Exp[0].Kind == "Var") {
        if (isReferenceDecl(decl)) {
            return exp.Exp[0];
        }
    }
    return exp;
}

function expressionIsVariable(exp, variable)
{
    return exp.Kind == "Var" && sameVariable(exp.Variable, variable);
}

// Similar to the above, except treat uses of a reference as if they were uses
// of the dereferenced contents. This requires knowing the type of the
// variable, and so takes its declaration rather than the variable itself.
function expressionIsDeclaredVariable(exp, decl)
{
    exp = maybeDereference(exp, decl);
    return expressionIsVariable(exp, decl.Variable);
}

function expressionIsMethodOnVariableDecl(exp, decl)
{
    // This might be calling a method on a base class, in which case exp will
    // be an unnamed field of the variable instead of the variable itself.
    while (exp.Kind == "Fld" && exp.Field.Name[0].startsWith("field:"))
        exp = exp.Exp[0];
    return expressionIsDeclaredVariable(exp, decl);
}

// Return whether the edge starts the live range of a variable's value, by setting
// it to some new value. Examples of starting obj's live range:
//
//     obj = foo;
//     obj = foo();
//     obj = foo(obj);         // uses previous value but then sets to new value
//     SomeClass obj(true, 1); // constructor
//
function edgeStartsValueLiveRange(edge, variable)
{
    // Direct assignments start live range of lhs: var = value
    if (edge.Kind == "Assign") {
        const [lhs, rhs] = edge.Exp;
        return (expressionIsVariable(lhs, variable) &&
                !isReturningImmobileValue(edge, variable));
    }

    if (edge.Kind != "Call")
        return false;

    // Assignments of call results start live range: var = foo()
    if (1 in edge.Exp) {
        var lhs = edge.Exp[1];
        if (expressionIsVariable(lhs, variable))
            return true;
    }

    // Constructor calls start live range of instance: SomeClass var(...)
    if ("PEdgeCallInstance" in edge) {
        var instance = edge.PEdgeCallInstance.Exp;

        // Kludge around incorrect dereference on some constructor calls.
        if (instance.Kind == "Drf")
            instance = instance.Exp[0];

        if (!expressionIsVariable(instance, variable))
            return false;

        var callee = edge.Exp[0];
        if (callee.Kind != "Var")
            return false;

        assert(callee.Variable.Kind == "Func");
        var calleeName = readable(callee.Variable.Name[0]);

        // Constructor calls include the text 'Name::Name(' or 'Name<...>::Name('.
        var openParen = calleeName.indexOf('(');
        if (openParen < 0)
            return false;
        calleeName = calleeName.substring(0, openParen);

        var lastColon = calleeName.lastIndexOf('::');
        if (lastColon < 0)
            return false;
        var constructorName = calleeName.substr(lastColon + 2);
        calleeName = calleeName.substr(0, lastColon);

        var lastTemplateOpen = calleeName.lastIndexOf('<');
        if (lastTemplateOpen >= 0)
            calleeName = calleeName.substr(0, lastTemplateOpen);

        if (calleeName.endsWith(constructorName))
            return true;
    }

    return false;
}

// Return the result of a `matcher` callback on the call found in the given
// `edge`, if the edge is a direct call to a named function (if not, return false).
// `matcher` is given the name of the callee (actually, a tuple
// [fully qualified name, base name]), an array of expressions containing the
// arguments, and if the result of the call is assigned to a variable,
// the expression representing that variable(the lhs).
//
// https://firefox-source-docs.mozilla.org/js/HazardAnalysis/CFG.html for
// documentation of the data structure used here.
function matchEdgeCall(edge, matcher) {
    if (edge.Kind != "Call") {
        return false;
    }

    const callee = edge.Exp[0];

    if (edge.Type.Kind == 'Function' &&
        edge.Exp[0].Kind == 'Var' &&
        edge.Exp[0].Variable.Kind == 'Func') {
        const calleeName = edge.Exp[0].Variable.Name;
        const args = edge.PEdgeCallArguments;
        const argExprs = args ? args.Exp : [];
        const lhs = edge.Exp[1]; // May be undefined
        return matcher(calleeName, argExprs, lhs);
    }

    return false;
}

function edgeMarksVariableGCSafe(edge, variable) {
    return matchEdgeCall(edge, (calleeName, argExprs, _lhs) => {
        // explicit JS_HAZ_VARIABLE_IS_GC_SAFE annotation
        return (calleeName[1] == 'MarkVariableAsGCSafe' &&
            calleeName[0].includes("JS::detail::MarkVariableAsGCSafe") &&
            argExprs.length == 1 &&
            expressionIsVariable(argExprs[0], variable));
    });
}

// Match an optional <namespace>:: followed by the class name,
// and then an optional template parameter marker.
//
// Example: mozilla::dom::UniquePtr<...
//
function parseTypeName(typeName) {
    const m = typeName.match(/^(((?:\w|::)+::)?(\w+))\b(\<)?/);
    if (!m) {
        return undefined;
    }
    const [, type, raw_namespace, classname, is_specialized] = m;
    const namespace = raw_namespace === null ? "" : raw_namespace;
    return { type, namespace, classname, is_specialized }
}

// Return whether an edge "clears out" a variable's value. A simple example
// would be
//
//     var = nullptr;
//
// for analyses for which nullptr is a "safe" value (eg GC rooting hazards; you
// can't get in trouble by holding a nullptr live across a GC.) A more complex
// example is a Maybe<T> that gets reset:
//
//     Maybe<AutoCheckCannotGC> nogc;
//     nogc.emplace(cx);
//     nogc.reset();
//     gc();             // <-- not a problem; nogc is invalidated by prev line
//     nogc.emplace(cx);
//     foo(nogc);
//
// Yet another example is a UniquePtr being passed by value, which means the
// receiver takes ownership:
//
//     UniquePtr<JSObject*> uobj(obj);
//     foo(uobj);
//     gc();
//
function edgeEndsValueLiveRange(edge, variable, body)
{
    // var = nullptr;
    if (edge.Kind == "Assign") {
        const [lhs, rhs] = edge.Exp;
        return expressionIsVariable(lhs, variable) && isImmobileValue(rhs);
    }

    if (edge.Kind != "Call")
        return false;

    if (edgeMarksVariableGCSafe(edge, variable)) {
        // explicit JS_HAZ_VARIABLE_IS_GC_SAFE annotation
        return true;
    }

    const decl = lookupVariable(body, variable);

    if (matchEdgeCall(edge, (calleeName, argExprs, lhs) => {
        return calleeName[1] == 'move' && calleeName[0].includes('std::move(') &&
            expressionIsDeclaredVariable(argExprs[0], decl) &&
            lhs &&
            lhs.Kind == 'Var' &&
            lhs.Variable.Kind == 'Temp';
    })) {
        // temp = std::move(var)
        //
        // If var is a UniquePtr, and we pass it into something that takes
        // ownership, then it should be considered to be invalid. Example:
        //
        //     consume(std::move(var));
        //
        // where consume takes a UniquePtr. This will compile to something like
        //
        //     UniquePtr* __temp_1 = &std::move(var);
        //     UniquePtr&& __temp_2(*temp_1); // move constructor
        //     consume(__temp_2);
        //     ~UniquePtr(__temp_2);
        //
        // The line commented with "// move constructor" is a result of passing
        // a UniquePtr as a parameter. If consume() took a UniquePtr&&
        // directly, this would just be:
        //
        //     UniquePtr* __temp_1 = &std::move(var);
        //     consume(__temp_1);
        //
        // which is not guaranteed to move from the reference. It might just
        // ignore the parameter. We can't predict what consume(UniquePtr&&)
        // will do. We do know that UniquePtr(UniquePtr&& other) moves out of
        // `other`.
        //
        // The std::move() technically is irrelevant, but because we only care
        // about bare variables, it has to be used, which is fortunate because
        // the UniquePtr&& constructor operates on a temporary, not the
        // variable we care about.

        const lhs = edge.Exp[1].Variable;
        if (basicBlockEatsVariable(lhs, body, edge.Index[1]))
          return true;
    }

    const callee = edge.Exp[0];

    if (edge.Type.Kind == 'Function' &&
        edge.Type.TypeFunctionCSU &&
        edge.PEdgeCallInstance &&
        expressionIsMethodOnVariableDecl(edge.PEdgeCallInstance.Exp, decl))
    {
        const typeName = edge.Type.TypeFunctionCSU.Type.Name;

        // Synthesize a zero-arg constructor name like
        // mozilla::dom::UniquePtr<T>::UniquePtr(). Note that the `<T>` is
        // literal -- the pretty name from sixgill will render the actual
        // constructor name as something like
        //
        //   UniquePtr<T>::UniquePtr() [where T = int]
        //
        const parsed = parseTypeName(typeName);
        if (parsed) {
            const { type, namespace, classname, is_specialized } = parsed;

            // special-case: the initial constructor that doesn't provide a value.
            // Useful for things like Maybe<T>.
            const template = is_specialized ? '<T>' : '';
            const ctorName = `${namespace}${classname}${template}::${classname}()`;
            if (callee.Kind == 'Var' &&
                typesWithSafeConstructors.has(type) &&
                callee.Variable.Name[0].includes(ctorName))
            {
                return true;
            }

            // special-case: UniquePtr::reset() and similar.
            if (callee.Kind == 'Var' &&
                type in resetterMethods &&
                resetterMethods[type].has(callee.Variable.Name[1]))
            {
                return true;
            }
        }
    }

    // special-case: passing UniquePtr<T> by value.
    if (edge.Type.Kind == 'Function' &&
        edge.Type.TypeFunctionArgument &&
        edge.PEdgeCallArguments)
    {
        for (const i in edge.Type.TypeFunctionArgument) {
            const param = edge.Type.TypeFunctionArgument[i];
            if (param.Type.Kind != 'CSU')
                continue;
            if (!param.Type.Name.startsWith("mozilla::UniquePtr<"))
                continue;
            const arg = edge.PEdgeCallArguments.Exp[i];
            if (expressionIsVariable(arg, variable)) {
                return true;
            }
        }
    }

    return false;
}

// Look up a variable in the list of declarations for this body.
function lookupVariable(body, variable) {
    for (const decl of (body.DefineVariable || [])) {
        if (sameVariable(decl.Variable, variable)) {
            return decl;
        }
    }
    return undefined;
}

function edgeMovesVariable(edge, variable, body)
{
    if (edge.Kind != 'Call')
        return false;
    const callee = edge.Exp[0];
    if (callee.Kind == 'Var' &&
        callee.Variable.Kind == 'Func')
    {
        const { Variable: { Name: [ fullname, shortname ] } } = callee;

        // Match an rvalue parameter.

        if (!edge || !edge.PEdgeCallArguments || !edge.PEdgeCallArguments.Exp) {
            return false;
        }

        for (const arg of edge.PEdgeCallArguments.Exp) {
            if (arg.Kind != 'Drf') continue;
            const val = arg.Exp[0];
            if (val.Kind == 'Var' && sameVariable(val.Variable, variable)) {
                // This argument is the variable we're looking for. Return true
                // if it is passed as an rvalue reference.
                const type = lookupVariable(body, variable).Type;
                if (type.Kind == "Pointer" && type.Reference == PTR_RVALUE_REF) {
                    return true;
                }
            }
        }
    }

    return false;
}

// Scan forward through the basic block in 'body' starting at 'startpoint',
// looking for a call that passes 'variable' to a move constructor that
// "consumes" it (eg UniquePtr::UniquePtr(UniquePtr&&)).
function basicBlockEatsVariable(variable, body, startpoint)
{
    const successors = getSuccessors(body);
    let point = startpoint;
    while (point in successors) {
        // Only handle a single basic block. If it forks, stop looking.
        const edges = successors[point];
        if (edges.length != 1) {
            return false;
        }
        const edge = edges[0];

        if (edgeMovesVariable(edge, variable, body)) {
            return true;
        }

        // edgeStartsValueLiveRange will find places where 'variable' is given
        // a new value. Never observed in practice, since this function is only
        // called with a temporary resulting from std::move(), which is used
        // immediately for a call. But just to be robust to future uses:
        if (edgeStartsValueLiveRange(edge, variable)) {
            return false;
        }

        point = edge.Index[1];
    }

    return false;
}

var PROP_REFCNT          = 1 << 0;
var PROP_SHARED_PTR_DTOR = 1 << 1;

function getCalleeProperties(calleeName) {
    let props = 0;

    if (isRefcountedDtor(calleeName)) {
        props |= PROP_REFCNT;
    }
    if (calleeName.includes("~shared_ptr()")) {
        props |= PROP_SHARED_PTR_DTOR;
    }
    return props;
}

// Basic C++ ABI mangling: prefix an identifier with its length, in decimal.
function mangle(name) {
    return name.length + name;
}

var TriviallyDestructibleTypes = new Set([
    // Single-token types from
    // https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling-builtin
    "void", "wchar_t", "bool", "char", "short", "int", "long", "float", "double",
    "__int64", "__int128", "__float128", "char32_t", "char16_t", "char8_t",
    // Remaining observed cases. These are types T in shared_ptr<T> that have
    // been observed, where the types themselves have trivial destructors, and
    // the custom deleter doesn't do anything nontrivial that we might care about.
    "_IO_FILE"
]);
function synthesizeDestructorName(className) {
    if (className.includes("<") || className.includes(" ") || className.includes("{")) {
        return;
    }
    if (TriviallyDestructibleTypes.has(className)) {
        return;
    }
    const parts = className.split("::");
    const mangled_dtor = "_ZN" + parts.map(p => mangle(p)).join("") + "D2Ev";
    const pretty_dtor = `void ${className}::~${parts.at(-1)}()`;
    // Note that there will be a later check to verify that the function name
    // synthesized here is an actual function, and assert if not (see
    // assertFunctionExists() in computeCallgraph.js.)
    return mangled_dtor + "$" + pretty_dtor;
}

function getCallEdgeProperties(body, edge, calleeName, functionBodies) {
    let attrs = 0;
    let extraCalls = [];

    if (edge.Kind !== "Call") {
        return { attrs, extraCalls };
    }

    const props = getCalleeProperties(calleeName);
    if (props & PROP_REFCNT) {
        // std::swap of two refcounted values thinks it can drop the
        // ref count to zero. Or rather, it just calls operator=() in a context
        // where the refcount will never drop to zero.
        const blockId = blockIdentifier(body);
        if (blockId.includes("std::swap") || blockId.includes("mozilla::Swap")) {
            // Replace the refcnt release call with nothing. It's not going to happen.
            attrs |= ATTR_REPLACED;
        }
    }

    if (props & PROP_SHARED_PTR_DTOR) {
        // Replace shared_ptr<T>::~shared_ptr() calls to T::~T() calls.
        // Note that this will only apply to simple cases.
        // Any templatized type, in particular, will be ignored and the original
        // call tree will be left alone. If this triggers a hazard, then we can
        // consider extending the mangling support.
        //
        // If the call to ~shared_ptr is not replaced, then it might end up calling
        // an unknown function pointer. This does not always happen-- in some cases,
        // the call tree below ~shared_ptr will invoke the correct destructor without
        // going through function pointers.
        const m = calleeName.match(/shared_ptr<(.*?)>::~shared_ptr\(\)(?: \[with T = ([\w:]+))?/);
        assert(m);
        let className = m[1] == "T" ? m[2] : m[1];
        assert(className != "");
        // cv qualification does not apply to destructors.
        className = className.replace("const ", "");
        className = className.replace("volatile ", "");
        const dtor = synthesizeDestructorName(className);
        if (dtor) {
            attrs |= ATTR_REPLACED;
            extraCalls.push({
                attrs: ATTR_SYNTHETIC,
                name: dtor,
            });
        }
    }

    if ((props & PROP_REFCNT) == 0) {
        return { attrs, extraCalls };
    }

    let callee = edge.Exp[0];
    while (callee.Kind === "Drf") {
        callee = callee.Exp[0];
    }

    const instance = edge.PEdgeCallInstance.Exp;
    if (instance.Kind !== "Var") {
        // TODO: handle field destructors
        return { attrs, extraCalls };
    }

    // Test whether the dtor call is dominated by operations on the variable
    // that mean it will not go to a zero refcount in the dtor: either because
    // it's already dead (eg r.forget() was called) or because it can be proven
    // to have a ref count of greater than 1. This is implemented by looking
    // for the reverse: find a path scanning backwards from the dtor call where
    // the variable is used in any way that does *not* ensure that it is
    // trivially destructible.

    const variable = instance.Variable;

    const visitor = new class DominatorVisitor extends Visitor {
        // Do not revisit nodes. For new nodes, relay the decision made by
        // extend_path.
        next_action(seen, current) { return seen ? "prune" : current; }

        // We don't revisit, so always use the new.
        merge_info(seen, current) { return current; }

        // Return the action to take from this node.
        extend_path(edge, body, ppoint, successor_value) {
            if (!edge) {
                // Dummy edge to join two points.
                return "continue";
            }

            if (!edgeUsesVariable(edge, variable, body)) {
                // Nothing of interest on this edge, keep searching.
                return "continue";
            }

            if (edgeEndsValueLiveRange(edge, variable, body)) {
                // This path is safe!
                return "prune";
            }

            // Unsafe. Found a use that might set the variable to a
            // nonzero refcount.
            return "done";
        }
    }(functionBodies);

    // Searching upwards from a destructor call, return the opposite of: is
    // there a path to a use or the start of the function that does NOT hit a
    // safe assignment like refptr.forget() first?
    //
    // In graph terms: return whether the destructor call is dominated by forget() calls (or similar).
    const edgeIsNonReleasingDtor = !BFS_upwards(
        body, edge.Index[0], functionBodies, visitor, "start",
        false // Return value if we do not reach the root without finding a non-forget() use.
    );
    if (edgeIsNonReleasingDtor) {
        attrs |= ATTR_GC_SUPPRESSED | ATTR_NONRELEASING;
    }
    return { attrs, extraCalls };
}

// gcc uses something like "__dt_del " for virtual destructors that it
// generates.
function isSyntheticVirtualDestructor(funcName) {
    return funcName.endsWith(" ");
}

function typedField(field)
{
    if ("FieldInstanceFunction" in field) {
        // Virtual call
        //
        // This makes a minimal attempt at dealing with overloading, by
        // incorporating the number of parameters. So far, that is all that has
        // been needed. If more is needed, sixgill will need to produce a full
        // mangled type.
        const {Type, Name: [name]} = field;

        // Virtual destructors don't need a type or argument count,
        // and synthetic ones don't have them filled in.
        if (isSyntheticVirtualDestructor(name)) {
            return name;
        }

        var nargs = 0;
        if (Type.Kind == "Function" && "TypeFunctionArguments" in Type)
            nargs = Type.TypeFunctionArguments.Type.length;
        return name + ":" + nargs;
    } else {
        // Function pointer field
        return field.Name[0];
    }
}

function fieldKey(csuName, field)
{
    return csuName + "." + typedField(field);
}
