/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

"use strict";

loadRelativeToScript('utility.js');
loadRelativeToScript('annotations.js');
loadRelativeToScript('CFG.js');

var sourceRoot = (os.getenv('SOURCE') || '') + '/'

var functionName;
var functionBodies;

if (typeof scriptArgs[0] != 'string' || typeof scriptArgs[1] != 'string')
    throw "Usage: analyzeRoots.js [-f function_name] <gcFunctions.lst> <gcEdges.txt> <suppressedFunctions.lst> <gcTypes.txt> [start end [tmpfile]]";

var theFunctionNameToFind;
if (scriptArgs[0] == '--function') {
    theFunctionNameToFind = scriptArgs[1];
    scriptArgs = scriptArgs.slice(2);
}

var gcFunctionsFile = scriptArgs[0];
var gcEdgesFile = scriptArgs[1];
var suppressedFunctionsFile = scriptArgs[2];
var gcTypesFile = scriptArgs[3];
var batch = (scriptArgs[4]|0) || 1;
var numBatches = (scriptArgs[5]|0) || 1;
var tmpfile = scriptArgs[6] || "tmp.txt";

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
    else if (type.Kind == "Array")
        return isUnrootedType(type.Type);
    else if (type.Kind == "CSU")
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

// If the edge uses the given variable, return the earliest point at which the
// use is definite. Usually, that means the source of the edge (anything that
// reaches that source point will end up using the variable, but there may be
// other ways to reach the destination of the edge.)
//
// Return values are implicitly used at the very last point in the function.
// This makes a difference: if an RAII class GCs in its destructor, we need to
// start looking at the final point in the function, not one point back from
// that, since that would skip over the GCing call.
//
function edgeUsesVariable(edge, variable, body)
{
    if (ignoreEdgeUse(edge, variable))
        return 0;

    if (variable.Kind == "Return" && body.Index[1] == edge.Index[1] && body.BlockId.Kind == "Function")
        return edge.Index[1]; // Last point in function body uses the return value.

    var src = edge.Index[0];

    switch (edge.Kind) {

    case "Assign":
        if (isReturningImmobileValue(edge, variable))
            return 0;
        if (expressionUsesVariable(edge.Exp[0], variable))
            return src;
        return expressionUsesVariable(edge.Exp[1], variable) ? src : 0;

    case "Assume":
        return expressionUsesVariableContents(edge.Exp[0], variable) ? src : 0;

    case "Call":
        if (expressionUsesVariable(edge.Exp[0], variable))
            return src;
        if (1 in edge.Exp && expressionUsesVariable(edge.Exp[1], variable))
            return src;
        if ("PEdgeCallInstance" in edge) {
            if (expressionUsesVariable(edge.PEdgeCallInstance.Exp, variable))
                return src;
        }
        if ("PEdgeCallArguments" in edge) {
            for (var exp of edge.PEdgeCallArguments.Exp) {
                if (expressionUsesVariable(exp, variable))
                    return src;
            }
        }
        return 0;

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

function edgeTakesVariableAddress(edge, variable)
{
    if (ignoreEdgeUse(edge, variable))
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

function edgeKillsVariable(edge, variable)
{
    // Direct assignments kill their lhs: var = value
    if (edge.Kind == "Assign") {
        var lhs = edge.Exp[0];
        if (lhs.Kind == "Var" && sameVariable(lhs.Variable, variable))
            return !isReturningImmobileValue(edge, variable);
    }

    if (edge.Kind != "Call")
        return false;

    // Assignments of call results kill their lhs.
    if (1 in edge.Exp) {
        var lhs = edge.Exp[1];
        if (lhs.Kind == "Var" && sameVariable(lhs.Variable, variable))
            return true;
    }

    // Constructor calls kill their 'this' value.
    if ("PEdgeCallInstance" in edge) {
        do {
            var instance = edge.PEdgeCallInstance.Exp;

            // Kludge around incorrect dereference on some constructor calls.
            if (instance.Kind == "Drf")
                instance = instance.Exp[0];

            if (instance.Kind != "Var" || !sameVariable(instance.Variable, variable))
                break;

            var callee = edge.Exp[0];
            if (callee.Kind != "Var")
                break;

            assert(callee.Variable.Kind == "Func");
            var calleeName = readable(callee.Variable.Name[0]);

            // Constructor calls include the text 'Name::Name(' or 'Name<...>::Name('.
            var openParen = calleeName.indexOf('(');
            if (openParen < 0)
                break;
            calleeName = calleeName.substring(0, openParen);

            var lastColon = calleeName.lastIndexOf('::');
            if (lastColon < 0)
                break;
            var constructorName = calleeName.substr(lastColon + 2);
            calleeName = calleeName.substr(0, lastColon);

            var lastTemplateOpen = calleeName.lastIndexOf('<');
            if (lastTemplateOpen >= 0)
                calleeName = calleeName.substr(0, lastTemplateOpen);

            if (calleeName.endsWith(constructorName))
                return true;
        } while (false);
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

// Search recursively through predecessors from a variable use, returning
// whether a GC call is reachable (in the reverse direction; this means that
// the variable use is reachable from the GC call, and therefore the variable
// is live after the GC call), along with some additional information. What
// info we want depends on whether the variable turns out to be live across any
// GC call. We are looking for both hazards (unrooted variables live across GC
// calls) and unnecessary roots (rooted variables that have no GC calls in
// their live ranges.)
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
function findGCBeforeVariableUse(suppressed, variable, worklist)
{
    // Scan through all edges preceding an unrooted variable use, using an
    // explicit worklist, looking for a GC call. A worklist contains an
    // incoming edge together with a description of where it or one of its
    // successors GC'd (if any).

    while (worklist.length) {
        var entry = worklist.pop();
        var { body, ppoint, gcInfo } = entry;

        if (body.seen) {
            if (ppoint in body.seen) {
                var seenEntry = body.seen[ppoint];
                if (!gcInfo || seenEntry.gcInfo)
                    continue;
            }
        } else {
            body.seen = [];
        }
        body.seen[ppoint] = {body: body, gcInfo: gcInfo};

        if (ppoint == body.Index[0]) {
            if (body.BlockId.Kind == "Loop") {
                // propagate to parents that enter the loop body.
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
            } else if (variable.Kind == "Arg" && gcInfo) {
                // The scope of arguments starts at the beginning of the
                // function
                return {gcInfo: gcInfo, why: entry};
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
                    return {gcInfo: gcInfo, why: {body: body, ppoint: source, gcInfo: gcInfo, why: entry } }

                // Otherwise, we want to continue searching for the true
                // minimumUse, for use in reporting unnecessary rooting, but we
                // truncate this particular branch of the search at this edge.
                continue;
            }

            if (!gcInfo && !(source in body.suppressed) && !suppressed) {
                var gcName = edgeCanGC(edge, body);
                if (gcName)
                    gcInfo = {name:gcName, body:body, ppoint:source};
            }

            if (edge_uses) {
                // The live range starts at least this far back, so we're done
                // for the same reason as with edge_kills.
                if (gcInfo)
                    return {gcInfo:gcInfo, why:entry};
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
                                       gcInfo:gcInfo, why:entry});
                    }
                }
                assert(found);
                break;
            }

            // Propagate the search to the predecessors of this edge.
            worklist.push({body:body, ppoint:source, gcInfo:gcInfo, why:entry});
        }
    }

    return null;
}

function variableLiveAcrossGC(suppressed, variable)
{
    // A variable is live across a GC if (1) it is used by an edge, and (2) it
    // is used after a GC in a successor edge.

    for (var body of functionBodies) {
        body.seen = null;
        body.minimumUse = 0;
    }

    for (var body of functionBodies) {
        if (!("PEdge" in body))
            continue;
        for (var edge of body.PEdge) {
            var usePoint = edgeUsesVariable(edge, variable, body);
            // Example for !edgeKillsVariable:
            //
            //   JSObject* obj = NewObject();
            //   cangc();
            //   obj = NewObject();    <-- uses 'obj', but kills previous value
            //
            if (usePoint && !edgeKillsVariable(edge, variable)) {
                // Found a use, possibly after a GC.
                var worklist = [{body:body, ppoint:usePoint, gcInfo:null, why:null}];
                var call = findGCBeforeVariableUse(suppressed, variable, worklist);
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
            if (edgeTakesVariableAddress(edge, variable)) {
                if (edge.Kind == "Assign" || (!suppressed && edgeCanGC(edge)))
                    return {body:body, ppoint:edge.Index[0]};
            }
        }
    }
    return null;
}

function computePrintedLines(functionName)
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

function findLocation(body, ppoint)
{
    var location = body.PPoint[ppoint - 1].Location;
    var text = location.CacheString + ":" + location.Line;
    if (text.indexOf(sourceRoot) == 0)
        return text.substring(sourceRoot.length);
    return text;
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
        computePrintedLines(functionName);

    while (entry) {
        var ppoint = entry.ppoint;
        var lineText = findLocation(entry.body, ppoint);

        var edgeText = "";
        if (entry.why && entry.why.body == entry.body) {
            // If the next point in the trace is in the same block, look for an edge between them.
            var next = entry.why.ppoint;

            if (!entry.body.edgeTable) {
                var table = {};
                entry.body.edgeTable = table;
                for (var line of entry.body.lines) {
                    if (match = /\((\d+),(\d+),/.exec(line))
                        table[match[1] + "," + match[2]] = line; // May be multiple?
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
    return type.Kind == "CSU" && isRootedTypeName(type.Name);
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
                printEntryTrace(functionName, result.why);
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
var each = Math.floor(N/numBatches);
var start = minStream + each * (batch - 1);
var end = Math.min(minStream + each * batch - 1, maxStream);

// For debugging: Set this variable to the function name you're interested in
// debugging and run once. That will print out the nameIndex of that function.
// Insert that into the following statement to go directly to just that
// function. Add your debugging printouts or debugger; statements or whatever.
var theFunctionNameToFind;
// var start = end = 12345;

function process(name, json) {
    functionName = name;
    functionBodies = JSON.parse(json);

    for (var body of functionBodies)
        body.suppressed = [];
    for (var body of functionBodies) {
        for (var [pbody, id] of allRAIIGuardedCallPoints(body, isSuppressConstructor))
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
    process(functionName, json);
    xdb.free_string(data);
}
