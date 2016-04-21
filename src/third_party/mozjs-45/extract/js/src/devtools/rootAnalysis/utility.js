/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

"use strict";

// gcc appends this to mangled function names for "not in charge"
// constructors/destructors.
var internalMarker = " *INTERNAL* ";

function assert(x, msg)
{
    if (x)
        return;
    debugger;
    if (msg)
        throw "assertion failed: " + msg + "\n" + (Error().stack);
    else
        throw "assertion failed: " + (Error().stack);
}

function defined(x) {
    return x !== undefined;
}

function xprint(x, padding)
{
    if (!padding)
        padding = "";
    if (x instanceof Array) {
        print(padding + "[");
        for (var elem of x)
            xprint(elem, padding + " ");
        print(padding + "]");
    } else if (x instanceof Object) {
        print(padding + "{");
        for (var prop in x) {
            print(padding + " " + prop + ":");
            xprint(x[prop], padding + "  ");
        }
        print(padding + "}");
    } else {
        print(padding + x);
    }
}

function sameBlockId(id0, id1)
{
    if (id0.Kind != id1.Kind)
        return false;
    if (!sameVariable(id0.Variable, id1.Variable))
        return false;
    if (id0.Kind == "Loop" && id0.Loop != id1.Loop)
        return false;
    return true;
}

function sameVariable(var0, var1)
{
    assert("Name" in var0 || var0.Kind == "This" || var0.Kind == "Return");
    assert("Name" in var1 || var1.Kind == "This" || var1.Kind == "Return");
    if ("Name" in var0)
        return "Name" in var1 && var0.Name[0] == var1.Name[0];
    return var0.Kind == var1.Kind;
}

function blockIdentifier(body)
{
    if (body.BlockId.Kind == "Loop")
        return body.BlockId.Loop;
    assert(body.BlockId.Kind == "Function", "body.Kind should be Function, not " + body.BlockId.Kind);
    return body.BlockId.Variable.Name[0];
}

function collectBodyEdges(body)
{
    body.predecessors = [];
    body.successors = [];
    if (!("PEdge" in body))
        return;

    for (var edge of body.PEdge) {
        var [ source, target ] = edge.Index;
        if (!(target in body.predecessors))
            body.predecessors[target] = [];
        body.predecessors[target].push(edge);
        if (!(source in body.successors))
            body.successors[source] = [];
        body.successors[source].push(edge);
    }
}

function getPredecessors(body)
{
    try {
        if (!('predecessors' in body))
            collectBodyEdges(body);
    } catch (e) {
        debugger;
        printErr("body is " + body);
    }
    return body.predecessors;
}

function getSuccessors(body)
{
    if (!('successors' in body))
        collectBodyEdges(body);
    return body.successors;
}

// Split apart a function from sixgill into its mangled and unmangled name. If
// no mangled name was given, use the unmangled name as its mangled name
function splitFunction(func)
{
    var split = func.indexOf("$");
    if (split != -1)
        return [ func.substr(0, split), func.substr(split+1) ];
    split = func.indexOf("|");
    if (split != -1)
        return [ func.substr(0, split), func.substr(split+1) ];
    return [ func, func ];
}

function mangled(fullname)
{
    var [ mangled, unmangled ] = splitFunction(fullname);
    return mangled;
}

function readable(fullname)
{
    var [ mangled, unmangled ] = splitFunction(fullname);
    return unmangled;
}

function xdbLibrary()
{
    var lib = ctypes.open(os.getenv('XDB'));
    return {
        open: lib.declare("xdb_open", ctypes.default_abi, ctypes.void_t, ctypes.char.ptr),
        min_data_stream: lib.declare("xdb_min_data_stream", ctypes.default_abi, ctypes.int),
        max_data_stream: lib.declare("xdb_max_data_stream", ctypes.default_abi, ctypes.int),
        read_key: lib.declare("xdb_read_key", ctypes.default_abi, ctypes.char.ptr, ctypes.int),
        read_entry: lib.declare("xdb_read_entry", ctypes.default_abi, ctypes.char.ptr, ctypes.char.ptr),
        free_string: lib.declare("xdb_free", ctypes.default_abi, ctypes.void_t, ctypes.char.ptr)
    };
}

function cLibrary()
{
    var lib;
    try {
        lib = ctypes.open("libc.so.6");
    } catch(e) {
        lib = ctypes.open("libc.so");
    }

    return {
        fopen: lib.declare("fopen", ctypes.default_abi, ctypes.void_t.ptr, ctypes.char.ptr, ctypes.char.ptr),
        getline: lib.declare("getline", ctypes.default_abi, ctypes.ssize_t, ctypes.char.ptr.ptr, ctypes.size_t.ptr, ctypes.void_t.ptr),
        fclose: lib.declare("fclose", ctypes.default_abi, ctypes.int, ctypes.void_t.ptr),
        free: lib.declare("free", ctypes.default_abi, ctypes.void_t, ctypes.void_t.ptr),
    };
}

function* readFileLines_gen(filename)
{
    var libc = cLibrary();
    var linebuf = ctypes.char.ptr();
    var bufsize = ctypes.size_t(0);
    var fp = libc.fopen(filename, "r");
    if (fp.isNull())
        throw "Unable to open '" + filename + "'"

    while (libc.getline(linebuf.address(), bufsize.address(), fp) > 0)
        yield linebuf.readString();
    libc.fclose(fp);
    libc.free(ctypes.void_t.ptr(linebuf));
}
