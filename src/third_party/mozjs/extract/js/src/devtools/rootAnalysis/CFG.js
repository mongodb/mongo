/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

// Utility code for traversing the JSON data structures produced by sixgill.

"use strict";

// Find all points (positions within the code) of the body given by the list of
// bodies and the blockId to match (which will specify an outer function or a
// loop within it), recursing into loops if needed.
function findAllPoints(bodies, blockId, limits)
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
        points.push([body, edge.Index[0], limits]);
        if (edge.Kind == "Loop")
            points.push(...findAllPoints(bodies, edge.BlockId, limits));
    }

    return points;
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
        const limits = isConstructor(typeInfo, edge.Type, variable.Name);
        if (!limits)
            continue;
        if (!("PEdgeCallInstance" in edge))
            continue;
        if (edge.PEdgeCallInstance.Exp.Kind != "Var")
            continue;

        points.push(...pointsInRAIIScope(bodies, body, edge, limits));
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

function pointsInRAIIScope(bodies, body, constructorEdge, limits) {
    var seen = {};
    var worklist = [constructorEdge.Index[1]];
    var points = [];
    while (worklist.length) {
        var point = worklist.pop();
        if (point in seen)
            continue;
        seen[point] = true;
        points.push([body, point, limits]);
        var successors = getSuccessors(body);
        if (!(point in successors))
            continue;
        for (var nedge of successors[point]) {
            if (isMatchingDestructor(constructorEdge, nedge))
                continue;
            if (nedge.Kind == "Loop")
                points.push(...findAllPoints(bodies, nedge.BlockId, limits));
            worklist.push(nedge.Index[1]);
        }
    }

    return points;
}
