/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

"use strict";

loadRelativeToScript('utility.js');
loadRelativeToScript('annotations.js');
loadRelativeToScript('CFG.js');
loadRelativeToScript('dumpCFG.js');

var sourceRoot = (os.getenv('SOURCE') || '') + '/'

var functionName;
var functionBodies;

if (typeof scriptArgs[0] != 'string' || typeof scriptArgs[1] != 'string')
    throw "Usage: analyzeRoots.js [-f function_name] <gcFunctions.lst> <gcEdges.txt> <suppressedFunctions.lst> <gcTypes.txt> <typeInfo.txt> [start end [tmpfile]]";

var theFunctionNameToFind;
if (scriptArgs[0] == '--function' || scriptArgs[0] == '-f') {
    theFunctionNameToFind = scriptArgs[1];
    scriptArgs = scriptArgs.slice(2);
}

var gcFunctionsFile = scriptArgs[0] || "gcFunctions.lst";
var gcEdgesFile = scriptArgs[1] || "gcEdges.txt";
var suppressedFunctionsFile = scriptArgs[2] || "suppressedFunctions.lst";
var gcTypesFile = scriptArgs[3] || "gcTypes.txt";
var typeInfoFile = scriptArgs[4] || "typeInfo.txt";
var batch = (scriptArgs[5]|0) || 1;
var numBatches = (scriptArgs[6]|0) || 1;
var tmpfile = scriptArgs[7] || "tmp.txt";

var gcFunctions = {};
var text = snarf("gcFunctions.lst").split("\n");
assert(text.pop().length == 0);
for (var line of text)
    gcFunctions[mangled(line)] = true;

var suppressedFunctions = {};
var text = snarf(suppressedFunctionsFile).split("\n");
assert(text.pop().length == 0);
for (var line of text) {
    suppressedFunctions[line] = true;
}
text = null;

var typeInfo = loadTypeInfo(typeInfoFile);

var gcEdges = {};
text = snarf(gcEdgesFile).split('\n');
assert(text.pop().length == 0);
for (var line of text) {
    var [ block, edge, func ] = line.split(" || ");
    if (!(block in gcEdges))
        gcEdges[block] = {}
    gcEdges[block][edge] = func;
}
text = null;

var match;
var gcThings = {};
var gcPointers = {};

text = snarf(gcTypesFile).split("\n");
for (var line of text) {
    if (match = /^GCThing: (.*)/.exec(line))
        gcThings[match[1]] = true;
    if (match = /^GCPointer: (.*)/.exec(line))
        gcPointers[match[1]] = true;
}
text = null;

function isGCType(type)
{
    if (type.Kind == "CSU")
        return type.Name in gcThings;
    else if (type.Kind == "Array")
        return isGCType(type.Type);
    return false;
}

function isUnrootedType(type)
{
    if (type.Kind == "Pointer")
        return isGCType(type.Type);
    else if (type.Kind == "Array") {
        if (!type.Type) {
            printErr("Received Array Kind with no Type");
            printErr(JSON.stringify(type));
            printErr(getBacktrace({args: true, locals: true}));
        }
        return isUnrootedType(type.Type);
    } else if (type.Kind == "CSU")
        return type.Name in gcPointers;
    else
        return false;
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
            if (edge.Exp[1].Kind == "Int" && edge.Exp[1].String == "0") {
                return true;
            }
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
// Note that this returns true only if the variable's incoming value is used.
// So this would return false for 'obj':
//
//     obj = someFunction();
//
// but these would return true:
//
//     obj = someFunction(obj);
//     obj->foo = someFunction();
//
function edgeUsesVariable(edge, variable, body)
{
    if (ignoreEdgeUse(edge, variable, body))
        return 0;

    if (variable.Kind == "Return" && body.Index[1] == edge.Index[1] && body.BlockId.Kind == "Function")
        return edge.Index[1]; // Last point in function body uses the return value.

    var src = edge.Index[0];

    switch (edge.Kind) {

    case "Assign": {
        if (isReturningImmobileValue(edge, variable))
            return 0;
        const [lhs, rhs] = edge.Exp;
        if (expressionUsesVariable(rhs, variable))
            return src;
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
                if (edgeKillsVariable(edge, variable)) {
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

    default:
        assert(false);
    }
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

function expressionIsVariable(exp, variable)
{
    return exp.Kind == "Var" && sameVariable(exp.Variable, variable);
}

// Return whether the edge kills (overwrites) the variable's incoming value.
// Examples of killing 'obj':
//
//     obj = foo;
//     obj = foo();
//     obj = foo(obj);         // uses previous value but then kills it
//     SomeClass obj(true, 1); // constructor
//
function edgeKillsVariable(edge, variable)
{
    // Direct assignments kill their lhs: var = value
    if (edge.Kind == "Assign") {
        const [lhs] = edge.Exp;
        return (expressionIsVariable(lhs, variable) &&
                !isReturningImmobileValue(edge, variable));
    }

    if (edge.Kind != "Call")
        return false;

    // Assignments of call results kill their lhs.
    if (1 in edge.Exp) {
        var lhs = edge.Exp[1];
        if (expressionIsVariable(lhs, variable))
            return true;
    }

    // Constructor calls kill their 'this' value.
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

function edgeCanGC(edge)
{
    if (edge.Kind != "Call")
        return false;

    var callee = edge.Exp[0];

    while (callee.Kind == "Drf")
        callee = callee.Exp[0];

    if (callee.Kind == "Var") {
        var variable = callee.Variable;

        if (variable.Kind == "Func") {
            var callee = mangled(variable.Name[0]);
            if ((callee in gcFunctions) || ((callee + internalMarker) in gcFunctions))
                return "'" + variable.Name[0] + "'";
            return null;
        }

        var varName = variable.Name[0];
        return indirectCallCannotGC(functionName, varName) ? null : "*" + varName;
    }

    if (callee.Kind == "Fld") {
        var field = callee.Field;
        var csuName = field.FieldCSU.Type.Name;
        var fullFieldName = csuName + "." + field.Name[0];
        if (fieldCallCannotGC(csuName, fullFieldName))
            return null;
        return (fullFieldName in suppressedFunctions) ? null : fullFieldName;
    }
}

// Search recursively through predecessors from the use of a variable's value,
// returning whether a GC call is reachable (in the reverse direction; this
// means that the variable use is reachable from the GC call, and therefore the
// variable is live after the GC call), along with some additional information.
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
//  - 'why': a path from the GC call to a use of the variable after the GC
//    call, chained through a 'why' field in the returned edge descriptor
//
//  - 'gcInfo': a direct pointer to the GC call edge
//
function findGCBeforeValueUse(start_body, start_point, suppressed, variable)
{
    // Scan through all edges preceding an unrooted variable use, using an
    // explicit worklist, looking for a GC call. A worklist contains an
    // incoming edge together with a description of where it or one of its
    // successors GC'd (if any).

    var bodies_visited = new Map();

    let worklist = [{body: start_body, ppoint: start_point, preGCLive: false, gcInfo: null, why: null}];
    while (worklist.length) {
        // Grab an entry off of the worklist, representing a point within the
        // CFG identified by <body,ppoint>. If this point has a descendant
        // later in the CFG that can GC, gcInfo will be set to the information
        // about that GC call.

        var entry = worklist.pop();
        var { body, ppoint, gcInfo, preGCLive } = entry;

        // Handle the case where there are multiple ways to reach this point
        // (traversing backwards).
        var visited = bodies_visited.get(body);
        if (!visited)
            bodies_visited.set(body, visited = new Map());
        if (visited.has(ppoint)) {
            var seenEntry = visited.get(ppoint);

            // This point already knows how to GC through some other path, so
            // we have nothing new to learn. (The other path will consider the
            // predecessors.)
            if (seenEntry.gcInfo)
                continue;

            // If this worklist's entry doesn't know of any way to GC, then
            // there's no point in continuing the traversal through it. Perhaps
            // another edge will be found that *can* GC; otherwise, the first
            // route to the point will traverse through predecessors.
            //
            // Note that this means we may visit a point more than once, if the
            // first time we visit we don't have a known reachable GC call and
            // the second time we do.
            if (!gcInfo)
                continue;
        }
        visited.set(ppoint, {body: body, gcInfo: gcInfo});

        // Check for hitting the entry point of the current body (which may be
        // the outer function or a loop within it.)
        if (ppoint == body.Index[0]) {
            if (body.BlockId.Kind == "Loop") {
                // Propagate to outer body parents that enter the loop body.
                if ("BlockPPoint" in body) {
                    for (var parent of body.BlockPPoint) {
                        var found = false;
                        for (var xbody of functionBodies) {
                            if (sameBlockId(xbody.BlockId, parent.BlockId)) {
                                assert(!found);
                                found = true;
                                worklist.push({body: xbody, ppoint: parent.Index,
                                               gcInfo: gcInfo, why: entry});
                            }
                        }
                        assert(found);
                    }
                }

                // Also propagate to the *end* of this loop, for the previous
                // iteration.
                worklist.push({body: body, ppoint: body.Index[1],
                               gcInfo: gcInfo, why: entry});
            } else if (variable.Kind == "Arg" && gcInfo) {
                // The scope of arguments starts at the beginning of the
                // function
                return entry;
            } else if (entry.preGCLive) {
                // We didn't find a "good" explanation beginning of the live
                // range, but we do know the variable was live across the GC.
                // This can happen if the live range started when a variable is
                // used as a retparam.
                return entry;
            }
        }

        var predecessors = getPredecessors(body);
        if (!(ppoint in predecessors))
            continue;

        for (var edge of predecessors[ppoint]) {
            var source = edge.Index[0];

            var edge_kills = edgeKillsVariable(edge, variable);
            var edge_uses = edgeUsesVariable(edge, variable, body);

            if (edge_kills || edge_uses) {
                if (!body.minimumUse || source < body.minimumUse)
                    body.minimumUse = source;
            }

            if (edge_kills) {
                // This is a beginning of the variable's live range. If we can
                // reach a GC call from here, then we're done -- we have a path
                // from the beginning of the live range, through the GC call,
                // to a use after the GC call that proves its live range
                // extends at least that far.
                if (gcInfo)
                    return {body: body, ppoint: source, gcInfo: gcInfo, why: entry };

                // Otherwise, keep searching through the graph, but truncate
                // this particular branch of the search at this edge.
                continue;
            }

            var src_gcInfo = gcInfo;
            var src_preGCLive = preGCLive;
            if (!gcInfo && !(source in body.suppressed) && !suppressed) {
                var gcName = edgeCanGC(edge, body);
                if (gcName)
                    src_gcInfo = {name:gcName, body:body, ppoint:source};
            }

            if (edge_uses) {
                // The live range starts at least this far back, so we're done
                // for the same reason as with edge_kills. The only difference
                // is that a GC on this edge indicates a hazard, whereas if
                // we're killing a live range in the GC call then it's not live
                // *across* the call.
                //
                // However, we may want to generate a longer usage chain for
                // the variable than is minimally necessary. For example,
                // consider:
                //
                //   Value v = f();
                //   if (v.isUndefined())
                //     return false;
                //   gc();
                //   return v;
                //
                // The call to .isUndefined() is considered to be a use and
                // therefore indicates that v must be live at that point. But
                // it's more helpful to the user to continue the 'why' path to
                // include the ancestor where the value was generated. So we
                // will only return here if edge.Kind is Assign; otherwise,
                // we'll pass a "preGCLive" value up through the worklist to
                // remember that the variable *is* alive before the GC and so
                // this function should be returning a true value even if we
                // don't find an assignment.

                if (src_gcInfo) {
                    src_preGCLive = true;
                    if (edge.Kind == 'Assign')
                        return {body:body, ppoint:source, gcInfo:src_gcInfo, why:entry};
                }
            }

            if (edge.Kind == "Loop") {
                // Additionally propagate the search into a loop body, starting
                // with the exit point.
                var found = false;
                for (var xbody of functionBodies) {
                    if (sameBlockId(xbody.BlockId, edge.BlockId)) {
                        assert(!found);
                        found = true;
                        worklist.push({body:xbody, ppoint:xbody.Index[1],
                                       preGCLive: src_preGCLive, gcInfo:src_gcInfo,
                                       why:entry});
                    }
                }
                assert(found);
                // Don't continue to predecessors here without going through
                // the loop. (The points in this body that enter the loop will
                // be traversed when we reach the entry point of the loop.)
                break;
            }

            // Propagate the search to the predecessors of this edge.
            worklist.push({body:body, ppoint:source,
                           preGCLive: src_preGCLive, gcInfo:src_gcInfo,
                           why:entry});
        }
    }

    return null;
}

function variableLiveAcrossGC(suppressed, variable)
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

            var usePoint = edgeUsesVariable(edge, variable, body);
            if (usePoint) {
                var call = findGCBeforeValueUse(body, usePoint, suppressed, variable);
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
function unsafeVariableAddressTaken(suppressed, variable)
{
    for (var body of functionBodies) {
        if (!("PEdge" in body))
            continue;
        for (var edge of body.PEdge) {
            if (edgeTakesVariableAddress(edge, variable, body)) {
                if (edge.Kind == "Assign" || (!suppressed && edgeCanGC(edge)))
                    return {body:body, ppoint:edge.Index[0]};
            }
        }
    }
    return null;
}

// Read out the brief (non-JSON, semi-human-readable) CFG description for the
// given function and store it.
function loadPrintedLines(functionName)
{
    assert(!os.system("xdbfind src_body.xdb '" + functionName + "' > " + tmpfile));
    var lines = snarf(tmpfile).split('\n');

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
    var location = body.PPoint[ppoint - 1].Location;
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

function printEntryTrace(functionName, entry)
{
    var gcPoint = entry.gcInfo ? entry.gcInfo.ppoint : 0;

    if (!functionBodies[0].lines)
        loadPrintedLines(functionName);

    while (entry) {
        var ppoint = entry.ppoint;
        var lineText = findLocation(entry.body, ppoint, {"brief": true});

        var edgeText = "";
        if (entry.why && entry.why.body == entry.body) {
            // If the next point in the trace is in the same block, look for an edge between them.
            var next = entry.why.ppoint;

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

        print("    " + lineText + (edgeText.length ? ": " + edgeText : ""));
        entry = entry.why;
    }
}

function isRootedType(type)
{
    return type.Kind == "CSU" && ((type.Name in typeInfo.RootedPointers) ||
                                  (type.Name in typeInfo.RootedGCThings));
}

function typeDesc(type)
{
    if (type.Kind == "CSU") {
        return type.Name;
    } else if ('Type' in type) {
        var inner = typeDesc(type.Type);
        if (type.Kind == 'Pointer')
            return inner + '*';
        else if (type.Kind == 'Array')
            return inner + '[]';
        else
            return inner + '?';
    } else {
        return '???';
    }
}

function processBodies(functionName)
{
    if (!("DefineVariable" in functionBodies[0]))
        return;
    var suppressed = (mangled(functionName) in suppressedFunctions);
    for (var variable of functionBodies[0].DefineVariable) {
        var name;
        if (variable.Variable.Kind == "This")
            name = "this";
        else if (variable.Variable.Kind == "Return")
            name = "<returnvalue>";
        else
            name = variable.Variable.Name[0];

        if (isRootedType(variable.Type)) {
            if (!variableLiveAcrossGC(suppressed, variable.Variable)) {
                // The earliest use of the variable should be its constructor.
                var lineText;
                for (var body of functionBodies) {
                    if (body.minimumUse) {
                        var text = findLocation(body, body.minimumUse);
                        if (!lineText || locationLine(lineText) > locationLine(text))
                            lineText = text;
                    }
                }
                print("\nFunction '" + functionName + "'" +
                      " has unnecessary root '" + name + "' at " + lineText);
            }
        } else if (isUnrootedType(variable.Type)) {
            var result = variableLiveAcrossGC(suppressed, variable.Variable);
            if (result) {
                var lineText = findLocation(result.gcInfo.body, result.gcInfo.ppoint);
                print("\nFunction '" + functionName + "'" +
                      " has unrooted '" + name + "'" +
                      " of type '" + typeDesc(variable.Type) + "'" +
                      " live across GC call " + result.gcInfo.name +
                      " at " + lineText);
                printEntryTrace(functionName, result);
            }
            result = unsafeVariableAddressTaken(suppressed, variable.Variable);
            if (result) {
                var lineText = findLocation(result.body, result.ppoint);
                print("\nFunction '" + functionName + "'" +
                      " takes unsafe address of unrooted '" + name + "'" +
                      " at " + lineText);
                printEntryTrace(functionName, {body:result.body, ppoint:result.ppoint});
            }
        }
    }
}

if (batch == 1)
    print("Time: " + new Date);

var xdb = xdbLibrary();
xdb.open("src_body.xdb");

var minStream = xdb.min_data_stream()|0;
var maxStream = xdb.max_data_stream()|0;

var N = (maxStream - minStream) + 1;
var start = Math.floor((batch - 1) / numBatches * N) + minStream;
var start_next = Math.floor(batch / numBatches * N) + minStream;
var end = start_next - 1;

function process(name, json) {
    functionName = name;
    functionBodies = JSON.parse(json);

    for (var body of functionBodies)
        body.suppressed = [];
    for (var body of functionBodies) {
        for (var [pbody, id] of allRAIIGuardedCallPoints(typeInfo, functionBodies, body, isSuppressConstructor))
            pbody.suppressed[id] = true;
    }
    processBodies(functionName);
}

if (theFunctionNameToFind) {
    var data = xdb.read_entry(theFunctionNameToFind);
    var json = data.readString();
    process(theFunctionNameToFind, json);
    xdb.free_string(data);
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
