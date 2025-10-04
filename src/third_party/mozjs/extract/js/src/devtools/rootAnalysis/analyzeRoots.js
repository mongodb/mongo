/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

"use strict";

loadRelativeToScript('utility.js');
loadRelativeToScript('annotations.js');
loadRelativeToScript('callgraph.js');
loadRelativeToScript('CFG.js');
loadRelativeToScript('dumpCFG.js');

var sourceRoot = (os.getenv('SOURCE') || '') + '/';

var functionName;
var functionBodies;

try {
    var options = parse_options([
        {
            name: "--function",
            type: 'string',
        },
        {
            name: "-f",
            type: "string",
            dest: "function",
        },
        {
            name: "gcFunctions",
            default: "gcFunctions.lst"
        },
        {
            name: "limitedFunctions",
            default: "limitedFunctions.lst"
        },
        {
            name: "gcTypes",
            default: "gcTypes.txt"
        },
        {
            name: "typeInfo",
            default: "typeInfo.txt"
        },
        {
            name: "batch",
            type: "number",
            default: 1
        },
        {
            name: "numBatches",
            type: "number",
            default: 1
        },
        {
            name: "tmpfile",
            default: "tmp.txt"
        },
    ]);
} catch (e) {
    printErr(e);
    printErr("Usage: analyzeRoots.js [-f function_name] <gcFunctions.lst> <limitedFunctions.lst> <gcTypes.txt> <typeInfo.txt> [start end [tmpfile]]");
    quit(1);
}
var gcFunctions = {};
var text = snarf(options.gcFunctions).split("\n");
assert(text.pop().length == 0);
for (const line of text)
    gcFunctions[mangled(line)] = readable(line);

var limitedFunctions = JSON.parse(snarf(options.limitedFunctions));
text = null;

var typeInfo = loadTypeInfo(options.typeInfo);

var match;
var gcThings = new Set();
var gcPointers = new Set();
var gcRefs = new Set(typeInfo.GCRefs);

text = snarf(options.gcTypes).split("\n");
for (var line of text) {
    if (match = /^GCThing: (.*)/.exec(line))
        gcThings.add(match[1]);
    if (match = /^GCPointer: (.*)/.exec(line))
        gcPointers.add(match[1]);
}
text = null;

function isGCRef(type)
{
    if (type.Kind == "CSU")
        return gcRefs.has(type.Name);
    return false;
}

function isGCType(type)
{
    if (type.Kind == "CSU")
        return gcThings.has(type.Name);
    else if (type.Kind == "Array")
        return isGCType(type.Type);
    return false;
}

function isUnrootedPointerDeclType(decl)
{
    // Treat non-temporary T& references as if they were the underlying type T.
    // For now, restrict this to only the types specifically annotated with JS_HAZ_GC_REF
    // to avoid lots of false positives with other types.
    let type = isReferenceDecl(decl) && isGCRef(decl.Type.Type) ? decl.Type.Type : decl.Type;

    while (type.Kind == "Array") {
        type = type.Type;
    }

    if (type.Kind == "Pointer") {
        return isGCType(type.Type);
    } else if (type.Kind == "CSU") {
        return gcPointers.has(type.Name);
    } else {
        return false;
    }
}

function edgeCanGC(functionName, body, edge, scopeAttrs, functionBodies)
{
    if (edge.Kind != "Call") {
        return false;
    }

    for (const { callee, attrs } of getCallees(body, edge, scopeAttrs, functionBodies)) {
        if (attrs & (ATTR_GC_SUPPRESSED | ATTR_REPLACED)) {
            continue;
        }

        if (callee.kind == "direct") {
            const func = mangled(callee.name);
            if ((func in gcFunctions) || ((func + internalMarker) in gcFunctions))
                return `'${func}$${gcFunctions[func]}'`;
            return false;
        } else if (callee.kind == "indirect") {
            if (!indirectCallCannotGC(functionName, callee.variable)) {
                return "'*" + callee.variable + "'";
            }
        } else if (callee.kind == "field") {
            if (fieldCallCannotGC(callee.staticCSU, callee.field)) {
                continue;
            }
            const fieldkey = callee.fieldKey;
            if (fieldkey in gcFunctions) {
                return `'${fieldkey}'`;
            }
        } else {
            return "<unknown>";
        }
    }

    return false;
}

// Search upwards through a function's control flow graph (CFG) to find a path containing:
//
// - a use of a variable, preceded by
//
// - a function call that can GC, preceded by
//
// - a use of the variable that shows that the live range starts at least that
//   far back, preceded by
//
// - an informative use of the variable (which might be the same use), one that
//   assigns to it a value that might contain a GC pointer (or is the start of
//   the function for parameters or 'this'.) This is not necessary for
//   correctness, it just makes it easier to understand why something might be
//   a hazard. The output of the analysis will include the whole path from the
//   informative use to the post-GC use, to make the problem as understandable
//   as possible.
//
// A canonical example might be:
//
//     void foo() {
//         JS::Value* val = lookupValue(); <-- informative use
//         if (!val.isUndefined()) {       <-- any use
//             GC();                       <-- GC call
//         }
//         putValue(val);                  <-- a use after a GC
//     }
//
// The search is performed on an underlying CFG that we traverse in
// breadth-first order (to find the shortest path). We build a path starting
// from an empty path and conditionally lengthening and improving it according
// to the computation occurring on each incoming edge. (If that path so far
// does not have a GC call and we traverse an edge with a GC call, then we
// lengthen the path by that edge and record it as including a GC call.) The
// resulting path may include a point or edge more than once! For example, in:
//
//     void foo(JS::Value val) {
//         for (int i = 0; i < N; i++) {
//             GC();
//             val = processValue(val);
//         }
//     }
//
// the path would start at the point after processValue(), go through the GC(),
// then back to the processValue() (for the call in the previous loop
// iteration).
//
// While searching, each point is annotated with a path node corresponding to
// the best path found to that node so far. When a later search ends up at the
// same point, the best path node is kept. (But the path that it heads may
// include an earlier path node for the same point, as in the case above.)
//
// What info we want depends on whether the variable turns out to be live
// across a GC call. We are looking for both hazards (unrooted variables live
// across GC calls) and unnecessary roots (rooted variables that have no GC
// calls in their live ranges.)
//
// If not:
//
//  - 'minimumUse': the earliest point in each body that uses the variable, for
//    reporting on unnecessary roots.
//
// If so:
//
//  - 'successor': a path from the GC call to a use of the variable after the GC
//    call, chained through 'successor' field in the returned edge descriptor
//
//  - 'gcInfo': a direct pointer to the GC call edge
//
function findGCBeforeValueUse(start_body, start_point, funcAttrs, variable)
{
    // Scan through all edges preceding an unrooted variable use, using an
    // explicit worklist, looking for a GC call and a preceding point where the
    // variable is known to be live. A worklist contains an incoming edge
    // together with a description of where it or one of its successors GC'd
    // (if any).

    class Path {
        get ProgressProperties() { return ["informativeUse", "anyUse", "gcInfo"]; }

        constructor(successor_path, body, ppoint) {
            Object.assign(this, {body, ppoint});
            if (successor_path !== undefined) {
                this.successor = successor_path;
                for (const prop of this.ProgressProperties) {
                    if (prop in successor_path) {
                        this[prop] = successor_path[prop];
                    }
                }
            }
        }

        toString() {
            const trail = [];
            for (let path = this; path.ppoint; path = path.successor) {
                trail.push(path.ppoint);
            }
            return trail.join();
        }

        // Return -1, 0, or 1 to indicate how complete this Path is compared
        // to another one.
        compare(other) {
            for (const prop of this.ProgressProperties) {
                const a = this.hasOwnProperty(prop);
                const b = other.hasOwnProperty(prop);
                if (a != b) {
                    return a - b;
                }
            }
            return 0;
        }
    };

    // In case we never find an informative use, keep track of the best path
    // found with any use.
    let bestPathWithAnyUse = null;

    const visitor = new class extends Visitor {
        constructor() {
            super(functionBodies);
        }

        // Do a BFS upwards through the CFG, starting from a use of the
        // variable and searching for a path containing a GC followed by an
        // initializing use of the variable (or, in forward direction, a start
        // of the variable's live range, a GC within that live range, and then
        // a use showing that the live range extends past the GC call.)
        // Actually, possibly two uses: any use at all, and then if available
        // an "informative" use that is more convincing (they may be the same).
        //
        // The CFG is a graph (a 'body' here is acyclic, but they can contain
        // loop nodes that bridge to additional bodies for the loop, so the
        // overall graph can by cyclic.) That means there may be multiple paths
        // from point A to point B, and we want paths with a GC on them. This
        // can be thought of as searching for a "maximal GCing" path from a use
        // A to an initialization B.
        //
        // This is implemented as a BFS search that when it reaches a point
        // that has been visited before, stops if and only if the current path
        // being advanced is a less GC-ful path. The traversal pushes a
        // `gcInfo` token, initially empty, up through the graph and stores the
        // maximal one visited so far at every point.
        //
        // Note that this means we may traverse through the same point more
        // than once, and so in theory this scan is superlinear -- if you visit
        // every point twice, once for a non GC path and once for a GC path, it
        // would be 2^n. But that is unlikely to matter, since you'd need lots
        // of split/join pairs that GC on one side and not the other, and you'd
        // have to visit them in an unlucky order. This could be fixed by
        // updating the gcInfo for past points in a path when a GC is found,
        // but it hasn't been found to matter in practice yet.

        next_action(prev, current) {
            // Continue if first visit, or the new path is more complete than the old path. This
            // could be enhanced at some point to choose paths with 'better'
            // examples of GC (eg a call that invokes GC through concrete functions rather than going through a function pointer that is conservatively assumed to GC.)

            if (!current) {
                // This search path has been terminated.
                return "prune";
            }

            if (current.informativeUse) {
                // We have a path with an informative use leading to a GC
                // leading to the starting point.
                assert(current.gcInfo);
                return "done";
            }

            if (prev === undefined) {
                // first visit
                return "continue";
            }

            if (!prev.gcInfo && current.gcInfo) {
                // More GC.
                return "continue";
            } else {
                return "prune";
            }
        }

        merge_info(prev, current) {
            // Keep the most complete path.

            if (!prev || !current) {
                return prev || current;
            }

            // Tie goes to the first found, since it will be shorter when doing a BFS-like search.
            return prev.compare(current) >= 0 ? prev : current;
        }

        extend_path(edge, body, ppoint, successor_path) {
            // Clone the successor path node and then tack on the new point. Other values
            // will be updated during the rest of this function, according to what is
            // happening on the edge.
            const path = new Path(successor_path, body, ppoint);
            if (edge === null) {
                // Artificial edge to connect loops to their surrounding nodes in the outer body.
                // Does not influence "completeness" of path.
                return path;
            }

            assert(ppoint == edge.Index[0]);

            if (edgeEndsValueLiveRange(edge, variable, body)) {
                // Terminate the search through this point.
                return null;
            }

            const edge_starts = edgeStartsValueLiveRange(edge, variable);
            const edge_uses = edgeUsesVariable(edge, variable, body);

            if (edge_starts || edge_uses) {
                if (!body.minimumUse || ppoint < body.minimumUse)
                    body.minimumUse = ppoint;
            }

            if (edge_starts) {
                // This is a beginning of the variable's live range. If we can
                // reach a GC call from here, then we're done -- we have a path
                // from the beginning of the live range, through the GC call, to a
                // use after the GC call that proves its live range extends at
                // least that far.
                if (path.gcInfo) {
                    path.anyUse = path.anyUse || edge;
                    path.informativeUse = path.informativeUse || edge;
                    return path;
                }

                // Otherwise, truncate this particular branch of the search at this
                // edge -- there is no GC after this use, and traversing the edge
                // would lead to a different live range.
                return null;
            }

            // The value is live across this edge. Check whether this edge can
            // GC (if we don't have a GC yet on this path.)
            const had_gcInfo = Boolean(path.gcInfo);
            const edgeAttrs = body.attrs[ppoint] | funcAttrs;
            if (!path.gcInfo && !(edgeAttrs & (ATTR_GC_SUPPRESSED | ATTR_REPLACED))) {
                var gcName = edgeCanGC(functionName, body, edge, edgeAttrs, functionBodies);
                if (gcName) {
                    path.gcInfo = {name:gcName, body, ppoint, edge: edge.Index};
                }
            }

            // Beginning of function?
            if (ppoint == body.Index[0] && body.BlockId.Kind != "Loop") {
                if (path.gcInfo && (variable.Kind == "Arg" || variable.Kind == "This")) {
                    // The scope of arguments starts at the beginning of the
                    // function.
                    path.anyUse = path.informativeUse = true;
                }

                if (path.anyUse) {
                    // We know the variable was live across the GC. We may or
                    // may not have found an "informative" explanation
                    // beginning of the live range. (This can happen if the
                    // live range started when a variable is used as a
                    // retparam.)
                    return path;
                }
            }

            if (!path.gcInfo) {
                // We haven't reached a GC yet, so don't start looking for uses.
                return path;
            }

            if (!edge_uses) {
                // We have a GC. If this edge doesn't use the value, then there
                // is no change to the completeness of the path.
                return path;
            }

            // The live range starts at least this far back, so we're done for
            // the same reason as with edge_starts. The only difference is that
            // a GC on this edge indicates a hazard, whereas if we're killing a
            // live range in the GC call then it's not live *across* the call.
            //
            // However, we may want to generate a longer usage chain for the
            // variable than is minimally necessary. For example, consider:
            //
            //   Value v = f();
            //   if (v.isUndefined())
            //     return false;
            //   gc();
            //   return v;
            //
            // The call to .isUndefined() is considered to be a use and
            // therefore indicates that v must be live at that point. But it's
            // more helpful to the user to continue the 'successor' path to
            // include the ancestor where the value was generated. So we will
            // only stop here if edge.Kind is Assign; otherwise, we'll pass a
            // "preGCLive" value up through the worklist to remember that the
            // variable *is* alive before the GC and so this function should be
            // returning a true value even if we don't find an assignment.

            // One special case: if the use of the variable is on the
            // destination part of the edge (which currently only happens for
            // the return value and a terminal edge in the body), and this edge
            // is also GCing, then that usage happens *after* the GC and so
            // should not be used for anyUse or informativeUse. This matters
            // for a hazard involving a destructor GC'ing after an immobile
            // return value has been assigned:
            //
            //   GCInDestructor guard(cx);
            //   if (cond()) {
            //     return nullptr;
            //   }
            //
            // which boils down to
            //
            //   p1 --(construct guard)-->
            //   p2 --(call cond)-->
            //   p3 --(returnval := nullptr) -->
            //   p4 --(destruct guard, possibly GCing)-->
            //   p5
            //
            // The return value is considered to be live at p5. The live range
            // of the return value would ordinarily be from p3->p4->p5, except
            // that the nullptr assignment means it needn't be considered live
            // back that far, and so the live range is *just* p5. The GC on the
            // 4->5 edge happens just before that range, so the value was not
            // live across the GC.
            //
            if (!had_gcInfo && edge_uses == edge.Index[1]) {
                return path; // New GC does not cross this variable use.
            }

            path.anyUse = path.anyUse || edge;
            bestPathWithAnyUse = bestPathWithAnyUse || path;
            if (edge.Kind == 'Assign') {
                path.informativeUse = edge; // Done! Setting this terminates the search.
            }

            return path;
        };
    };

    const result = BFS_upwards(start_body, start_point, functionBodies, visitor, new Path());
    if (result && result.gcInfo && result.anyUse) {
        return result;
    } else {
        return bestPathWithAnyUse;
    }
}

function variableLiveAcrossGC(funcAttrs, variable, liveToEnd=false)
{
    // A variable is live across a GC if (1) it is used by an edge (as in, it
    // was at least initialized), and (2) it is used after a GC in a successor
    // edge.

    for (var body of functionBodies)
        body.minimumUse = 0;

    for (var body of functionBodies) {
        if (!("PEdge" in body))
            continue;
        for (var edge of body.PEdge) {
            // Examples:
            //
            //   JSObject* obj = NewObject();
            //   cangc();
            //   obj = NewObject();     <-- mentions 'obj' but kills previous value
            //
            // This is not a hazard. Contrast this with:
            //
            //   JSObject* obj = NewObject();
            //   cangc();
            //   obj = LookAt(obj);  <-- uses 'obj' and kills previous value
            //
            // This is a hazard; the initial value of obj is live across
            // cangc(). And a third possibility:
            //
            //   JSObject* obj = NewObject();
            //   obj = CopyObject(obj);
            //
            // This is not a hazard, because even though CopyObject can GC, obj
            // is not live across it. (obj is live before CopyObject, and
            // probably after, but not across.) There may be a hazard within
            // CopyObject, of course.
            //

            // Ignore uses that are just invalidating the previous value.
            if (edgeEndsValueLiveRange(edge, variable, body))
                continue;

            var usePoint = edgeUsesVariable(edge, variable, body, liveToEnd);
            if (usePoint) {
                var call = findGCBeforeValueUse(body, usePoint, funcAttrs, variable);
                if (!call)
                    continue;

                call.afterGCUse = usePoint;
                return call;
            }
        }
    }
    return null;
}

// An unrooted variable has its address stored in another variable via
// assignment, or passed into a function that can GC. If the address is
// assigned into some other variable, we can't track it to see if it is held
// live across a GC. If it is passed into a function that can GC, then it's
// sort of like a Handle to an unrooted location, and the callee could GC
// before overwriting it or rooting it.
function unsafeVariableAddressTaken(funcAttrs, variable)
{
    for (var body of functionBodies) {
        if (!("PEdge" in body))
            continue;
        for (var edge of body.PEdge) {
            if (edgeTakesVariableAddress(edge, variable, body)) {
                if (funcAttrs & (ATTR_GC_SUPPRESSED | ATTR_REPLACED)) {
                    continue;
                }
                if (edge.Kind == "Assign" || edgeCanGC(functionName, body, edge, funcAttrs, functionBodies)) {
                    return {body:body, ppoint:edge.Index[0]};
                }
            }
        }
    }
    return null;
}

// Read out the brief (non-JSON, semi-human-readable) CFG description for the
// given function and store it.
function loadPrintedLines(functionName)
{
    assert(!os.system("xdbfind src_body.xdb '" + functionName + "' > " + options.tmpfile));
    var lines = snarf(options.tmpfile).split('\n');

    for (var body of functionBodies)
        body.lines = [];

    // Distribute lines of output to the block they originate from.
    var currentBody = null;
    for (var line of lines) {
        if (/^block:/.test(line)) {
            if (match = /:(loop#[\d#]+)/.exec(line)) {
                var loop = match[1];
                var found = false;
                for (var body of functionBodies) {
                    if (body.BlockId.Kind == "Loop" && body.BlockId.Loop == loop) {
                        assert(!found);
                        found = true;
                        currentBody = body;
                    }
                }
                assert(found);
            } else {
                for (var body of functionBodies) {
                    if (body.BlockId.Kind == "Function")
                        currentBody = body;
                }
            }
        }
        if (currentBody)
            currentBody.lines.push(line);
    }
}

function findLocation(body, ppoint, opts={brief: false})
{
    var location = body.PPoint[ppoint ? ppoint - 1 : 0].Location;
    var file = location.CacheString;

    if (file.indexOf(sourceRoot) == 0)
        file = file.substring(sourceRoot.length);

    if (opts.brief) {
        var m = /.*\/(.*)/.exec(file);
        if (m)
            file = m[1];
    }

    return file + ":" + location.Line;
}

function locationLine(text)
{
    if (match = /:(\d+)$/.exec(text))
        return match[1];
    return 0;
}

function getEntryTrace(functionName, entry)
{
    const trace = [];

    var gcPoint = entry.gcInfo ? entry.gcInfo.ppoint : 0;

    if (!functionBodies[0].lines)
        loadPrintedLines(functionName);

    while (entry.successor) {
        var ppoint = entry.ppoint;
        var lineText = findLocation(entry.body, ppoint, {"brief": true});

        var edgeText = "";
        if (entry.successor && entry.successor.body == entry.body) {
            // If the next point in the trace is in the same block, look for an
            // edge between them.
            var next = entry.successor.ppoint;

            if (!entry.body.edgeTable) {
                var table = {};
                entry.body.edgeTable = table;
                for (var line of entry.body.lines) {
                    if (match = /^\w+\((\d+,\d+),/.exec(line))
                        table[match[1]] = line; // May be multiple?
                }
                if (entry.body.BlockId.Kind == 'Loop') {
                    const [startPoint, endPoint] = entry.body.Index;
                    table[`${endPoint},${startPoint}`] = '(loop to next iteration)';
                }
            }

            edgeText = entry.body.edgeTable[ppoint + "," + next];
            assert(edgeText);
            if (ppoint == gcPoint)
                edgeText += " [[GC call]]";
        } else {
            // Look for any outgoing edge from the chosen point.
            for (var line of entry.body.lines) {
                if (match = /\((\d+),/.exec(line)) {
                    if (match[1] == ppoint) {
                        edgeText = line;
                        break;
                    }
                }
            }
            if (ppoint == entry.body.Index[1] && entry.body.BlockId.Kind == "Function")
                edgeText += " [[end of function]]";
        }

        // TODO: Store this in a more structured form for better markup, and perhaps
        // linking to line numbers.
        trace.push({lineText, edgeText});
        entry = entry.successor;
    }

    return trace;
}

function isRootedDeclType(decl)
{
    // Treat non-temporary T& references as if they were the underlying type T.
    const type = isReferenceDecl(decl) ? decl.Type.Type : decl.Type;
    return type.Kind == "CSU" && ((type.Name in typeInfo.RootedPointers) ||
                                  (type.Name in typeInfo.RootedGCThings));
}

function printRecord(record) {
    print(JSON.stringify(record));
}

function processBodies(functionName, wholeBodyAttrs)
{
    if (!("DefineVariable" in functionBodies[0]))
      return;
    const funcInfo = limitedFunctions[mangled(functionName)] || { attributes: 0 };
    const funcAttrs = funcInfo.attributes | wholeBodyAttrs;

    // Look for the JS_EXPECT_HAZARDS annotation, so as to output a different
    // message in that case that won't be counted as a hazard.
    var annotations = new Set();
    for (const variable of functionBodies[0].DefineVariable) {
        if (variable.Variable.Kind == "Func" && variable.Variable.Name[0] == functionName) {
            for (const { Name: [tag, value] } of (variable.Type.Annotation || [])) {
                if (tag == 'annotate')
                    annotations.add(value);
            }
        }
    }

    let missingExpectedHazard = annotations.has("Expect Hazards");

    // Awful special case, hopefully temporary:
    //
    // The DOM bindings code generator uses "holders" to externally root
    // variables. So for example:
    //
    //       StringObjectRecordOrLong arg0;
    //       StringObjectRecordOrLongArgument arg0_holder(arg0);
    //       arg0_holder.TrySetToStringObjectRecord(cx, args[0]);
    //       GC();
    //       self->PassUnion22(cx, arg0);
    //
    // This appears to be a rooting hazard on arg0, but it is rooted by
    // arg0_holder if you set it to any of its union types that requires
    // rooting.
    //
    // Additionally, the holder may be reported as a hazard because it's not
    // itself a Rooted or a subclass of AutoRooter; it contains a
    // Maybe<RecordRooter<T>> that will get emplaced if rooting is required.
    //
    // Hopefully these will be simplified at some point (see bug 1517829), but
    // for now we special-case functions in the mozilla::dom namespace that
    // contain locals with types ending in "Argument". Or
    // Maybe<SomethingArgument>. Or Maybe<SpiderMonkeyInterfaceRooter<T>>. It's
    // a harsh world.
    const ignoreVars = new Set();
    if (functionName.match(/mozilla::dom::/)) {
        const vars = functionBodies[0].DefineVariable.filter(
            v => v.Type.Kind == 'CSU' && v.Variable.Kind == 'Local'
        ).map(
            v => [ v.Variable.Name[0], v.Type.Name ]
        );

        const holders = vars.filter(
            ([n, t]) => n.match(/^arg\d+_holder$/) &&
                        (t.includes("Argument") || t.includes("Rooter")));
        for (const [holder,] of holders) {
            ignoreVars.add(holder); // Ignore the holder.
            ignoreVars.add(holder.replace("_holder", "")); // Ignore the "managed" arg.
        }
    }

    const [mangledSymbol, readable] = splitFunction(functionName);

    for (let decl of functionBodies[0].DefineVariable) {
        var name;
        if (decl.Variable.Kind == "This")
            name = "this";
        else if (decl.Variable.Kind == "Return")
            name = "<returnvalue>";
        else
            name = decl.Variable.Name[0];

        if (ignoreVars.has(name))
            continue;

        let liveToEnd = false;
        if (decl.Variable.Kind == "Arg" && isReferenceDecl(decl) && decl.Type.Reference == 2) {
            // References won't run destructors, so they would normally not be
            // considered live at the end of the function. In order to handle
            // the pattern of moving a GC-unsafe value into a function (eg an
            // AutoCheckCannotGC&&), assume all argument rvalue references live to the
            // end of the function unless their liveness is terminated by
            // calling reset() or moving them into another function call.
            liveToEnd = true;
        }

        if (isRootedDeclType(decl)) {
            if (!variableLiveAcrossGC(funcAttrs, decl.Variable)) {
                // The earliest use of the variable should be its constructor.
                var lineText;
                for (var body of functionBodies) {
                    if (body.minimumUse) {
                        var text = findLocation(body, body.minimumUse);
                        if (!lineText || locationLine(lineText) > locationLine(text))
                            lineText = text;
                    }
                }
                const record = {
                    record: "unnecessary",
                    functionName,
                    mangled: mangledSymbol,
                    readable,
                    variable: name,
                    type: str_Type(decl.Type),
                    loc: lineText || "???",
                }
                print(",");
                printRecord(record);
            }
        } else if (isUnrootedPointerDeclType(decl)) {
            var result = variableLiveAcrossGC(funcAttrs, decl.Variable, liveToEnd);
            if (result) {
                assert(result.gcInfo);
                const edge = result.gcInfo.edge;
                const body = result.gcInfo.body;
                const lineText = findLocation(body, result.gcInfo.ppoint);
                const makeLoc = l => [l.Location.CacheString, l.Location.Line];
                const range = [makeLoc(body.PPoint[edge[0] - 1]), makeLoc(body.PPoint[edge[1] - 1])];
                const record = {
                    record: "unrooted",
                    expected: annotations.has("Expect Hazards"),
                    functionName,
                    mangled: mangledSymbol,
                    readable,
                    variable: name,
                    type: str_Type(decl.Type),
                    gccall: result.gcInfo.name.replaceAll("'", ""),
                    gcrange: range,
                    loc: lineText,
                    trace: getEntryTrace(functionName, result),
                };
                missingExpectedHazard = false;
                print(",");
                printRecord(record);
            }
            result = unsafeVariableAddressTaken(funcAttrs, decl.Variable);
            if (result) {
                var lineText = findLocation(result.body, result.ppoint);
                const record = {
                    record: "address",
                    functionName,
                    mangled: mangledSymbol,
                    readable,
                    variable: name,
                    loc: lineText,
                    trace: getEntryTrace(functionName, {body:result.body, ppoint:result.ppoint}),
                };
                print(",");
                printRecord(record);
            }
        }
    }

    if (missingExpectedHazard) {
        const {
            Location: [
                { CacheString: startfile, Line: startline },
                { CacheString: endfile, Line: endline }
            ]
        } = functionBodies[0];

        const loc = (startfile == endfile) ? `${startfile}:${startline}-${endline}`
              : `${startfile}:${startline}`;

        const record = {
            record: "missing",
            functionName,
            mangled: mangledSymbol,
            readable,
            loc,
        }
        print(",");
        printRecord(record);
    }
}

print("[\n");
var now = new Date();
printRecord({record: "time", iso: "" + now, t: now.getTime()});

var xdb = xdbLibrary();
xdb.open("src_body.xdb");

var minStream = xdb.min_data_stream()|0;
var maxStream = xdb.max_data_stream()|0;

var start = batchStart(options.batch, options.numBatches, minStream, maxStream);
var end = batchLast(options.batch, options.numBatches, minStream, maxStream);

function process(name, json) {
    functionName = name;
    functionBodies = JSON.parse(json);

    // Annotate body with a table of all points within the body that may be in
    // a limited scope (eg within the scope of a GC suppression RAII class.)
    // body.attrs is a plain object indexed by point, with the value being a
    // bit set stored in an integer.
    for (var body of functionBodies)
        body.attrs = [];

    for (var body of functionBodies) {
        for (var [pbody, id, attrs] of allRAIIGuardedCallPoints(typeInfo, functionBodies, body, isLimitConstructor))
        {
            if (attrs)
                pbody.attrs[id] = attrs;
        }
    }

    processBodies(functionName);
}

if (options.function) {
    var data = xdb.read_entry(options.function);
    var json = data.readString();
    debugger;
    process(options.function, json);
    xdb.free_string(data);
    print("\n]\n");
    quit(0);
}

for (var nameIndex = start; nameIndex <= end; nameIndex++) {
    var name = xdb.read_key(nameIndex);
    var functionName = name.readString();
    var data = xdb.read_entry(name);
    xdb.free_string(name);
    var json = data.readString();
    try {
        process(functionName, json);
    } catch (e) {
        printErr("Exception caught while handling " + functionName);
        throw(e);
    }
    xdb.free_string(data);
}

print("\n]\n");
