/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

"use strict";

loadRelativeToScript('dumpCFG.js');

// Attribute bits - each call edge may carry a set of 'attrs' bits, saying eg
// that the edge takes place within a scope where GC is suppressed, for
// example.
var ATTR_GC_SUPPRESSED     = 1 << 0;
var ATTR_CANSCRIPT_BOUNDED = 1 << 1; // Unimplemented
var ATTR_DOM_ITERATING     = 1 << 2; // Unimplemented
var ATTR_NONRELEASING      = 1 << 3; // ~RefPtr of value whose refcount will not go to zero
var ATTR_REPLACED          = 1 << 4; // Ignore edge, it was replaced by zero or more better edges.
var ATTR_SYNTHETIC         = 1 << 5; // Call was manufactured in some way.

var ATTR_LAST              = 1 << 5;
var ATTRS_NONE             = 0;
var ATTRS_ALL              = (ATTR_LAST << 1) - 1; // All possible bits set

// The traversal algorithms we run will recurse into children if you change any
// attrs bit to zero. Use all bits set to maximally attributed, including
// additional bits that all just mean "unvisited", so that the first time we
// see a node with this attrs, we're guaranteed to turn at least one bit off
// and thereby keep going.
var ATTRS_UNVISITED = 0xffff;

// gcc appends this to mangled function names for "not in charge"
// constructors/destructors.
var internalMarker = " *INTERNAL* ";

if (! Set.prototype.hasOwnProperty("update")) {
    Object.defineProperty(Set.prototype, "update", {
        value: function (collection) {
            for (let elt of collection)
                this.add(elt);
        }
    });
}

function assert(x, msg)
{
    if (x)
        return;
    debugger;
    if (msg)
        throw new Error("assertion failed: " + msg + "\n");
    else
        throw new Error("assertion failed");
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

// Command-line argument parser.
//
// `parameters` is a dict of parameters specs, each of which is a dict with keys:
//
//   - name: name of option, prefixed with "--" if it is named (otherwise, it
//     is interpreted as a positional parameter.)
//   - dest: key to store the result in, defaulting to the parameter name without
//     any leading "--"" and with dashes replaced with underscores.
//   - default: value of option if no value is given. Positional parameters with
//     a default value are optional. If no default is given, the parameter's name
//     is not included in the return value.
//   - type: `bool` if it takes no argument, otherwise an argument is required.
//     Named arguments default to 'bool', positional arguments to 'string'.
//   - nargs: the only supported value is `+`, which means to grab all following
//     arguments, up to the next named option, and store them as a list.
//
// The command line is parsed for `--foo=value` and `--bar` arguments.
//
// Return value is a dict of parameter values, keyed off of `dest` as determined
// above. An extra option named "rest" will be set to the list of all remaining
// arguments passed in.
//
function parse_options(parameters, inArgs = scriptArgs) {
    const options = {};

    const named = {};
    const positional = [];
    for (const param of parameters) {
        if (param.name.startsWith("-")) {
            named[param.name] = param;
            if (!param.dest) {
                if (!param.name.startsWith("--")) {
                    throw new Error(`parameter '${param.name}' requires param.dest to be set`);
                }
                param.dest = param.name.substring(2).replace("-", "_");
            }
        } else {
            if (!('default' in param) && positional.length > 0 && ('default' in positional.at(-1))) {
                throw new Error(`required parameter '${param.name}' follows optional parameter`);
            }
            param.positional = true;
            positional.push(param);
            param.dest = param.dest || param.name.replace("-", "_");
        }

        if (!param.type) {
            if (param.nargs === "+") {
                param.type = "list";
            } else if (param.positional) {
                param.type = "string";
            } else {
                param.type = "bool";
            }
        }

        if ('default' in param) {
            options[param.dest] = param.default;
        }
    }

    options.rest = [];
    const args = [...inArgs];
    let grabbing_into = undefined;
    while (args.length > 0) {
        let arg = args.shift();
        let param;
        if (arg.startsWith("-") && arg in named) {
            param = named[arg];
            if (param.type !== 'bool') {
                if (args.length == 0) {
                    throw(new Error(`${param.name} requires an argument`));
                }
                arg = args.shift();
            }
        } else {
            const pos = arg.indexOf("=");
            if (pos != -1) {
                const name = arg.substring(0, pos);
                param = named[name];
                if (!param) {
                    throw(new Error(`Unknown option '${name}'`));
                } else if (param.type === 'bool') {
                    throw(new Error(`--${param.name} does not take an argument`));
                }
                arg = arg.substring(pos + 1);
            }
        }

        // If this isn't a --named param, and we're not accumulating into a nargs="+" param, then
        // use the next positional.
        if (!param && !grabbing_into && positional.length > 0) {
            param = positional.shift();
        }

        // If a parameter was identified, then any old accumulator is done and we might start a new one.
        if (param) {
            if (param.type === 'list') {
                grabbing_into = options[param.dest] = options[param.dest] || [];
            } else {
                grabbing_into = undefined;
            }
        }

        if (grabbing_into) {
            grabbing_into.push(arg);
        } else if (param) {
            if (param.type === 'bool') {
                options[param.dest] = true;
            } else {
                options[param.dest] = arg;
            }
        } else {
            options.rest.push(arg);
        }
    }

    for (const param of positional) {
        if (!('default' in param)) {
            throw(new Error(`'${param.name}' option is required`));
        }
    }

    for (const param of parameters) {
        if (param.nargs === '+' && options[param.dest].length == 0) {
            throw(new Error(`at least one value required for option '${param.name}'`));
        }
    }

    return options;
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
    if (!('predecessors' in body))
        collectBodyEdges(body);
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
    var api = {
        open: lib.declare("xdb_open", ctypes.default_abi, ctypes.void_t, ctypes.char.ptr),
        min_data_stream: lib.declare("xdb_min_data_stream", ctypes.default_abi, ctypes.int),
        max_data_stream: lib.declare("xdb_max_data_stream", ctypes.default_abi, ctypes.int),
        read_key: lib.declare("xdb_read_key", ctypes.default_abi, ctypes.char.ptr, ctypes.int),
        read_entry: lib.declare("xdb_read_entry", ctypes.default_abi, ctypes.char.ptr, ctypes.char.ptr),
        free_string: lib.declare("xdb_free", ctypes.default_abi, ctypes.void_t, ctypes.char.ptr)
    };
    try {
        api.lookup_key = lib.declare("xdb_lookup_key", ctypes.default_abi, ctypes.int, ctypes.char.ptr);
    } catch (e) {
        // lookup_key is for development use only and is not strictly necessary.
    }
    return api;
}

function openLibrary(names) {
    for (const name of names) {
        try {
            return ctypes.open(name);
        } catch(e) {
        }
    }
    return undefined;
}

function cLibrary()
{
    const lib = openLibrary(['libc.so.6', 'libc.so', 'libc.dylib']);
    if (!lib) {
        throw new Error("Unable to open libc");
    }

    if (getBuildConfiguration("moz-memory")) {
        throw new Error("cannot use libc functions with --enable-jemalloc, since they will be routed " +
                        "through jemalloc, but calling libc.free() directly will bypass it and the " +
                        "malloc/free will be mismatched");
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
        throw new Error("Unable to open '" + filename + "'");

    while (libc.getline(linebuf.address(), bufsize.address(), fp) > 0)
        yield linebuf.readString();
    libc.fclose(fp);
    libc.free(ctypes.void_t.ptr(linebuf));
}

function addToKeyedList(collection, key, entry)
{
    if (!(key in collection))
        collection[key] = [];
    collection[key].push(entry);
    return collection[key];
}

function addToMappedList(map, key, entry)
{
    if (!map.has(key))
        map.set(key, []);
    map.get(key).push(entry);
    return map.get(key);
}

function loadTypeInfo(filename)
{
    return JSON.parse(os.file.readFile(filename));
}

// Given the range `first` .. `last`, break it down into `count` batches and
// return the start of the (1-based) `num` batch.
function batchStart(num, count, first, last) {
  const N = (last - first) + 1;
  return Math.floor((num - 1) / count * N) + first;
}

// As above, but return the last value in the (1-based) `num` batch.
function batchLast(num, count, first, last) {
  const N = (last - first) + 1;
  return Math.floor(num / count * N) + first - 1;
}

// Debugging tool. See usage below.
function PropertyTracer(traced_prop, check) {
    return {
        matches(prop, value) {
            if (prop != traced_prop)
                return false;
            if ('value' in check)
                return value == check.value;
            return true;
        },

        // Also called when defining a property.
        set(obj, prop, value) {
            if (this.matches(prop, value))
                debugger;
            return Reflect.set(...arguments);
        },
    };
}

// Usage: var myobj = traced({}, 'name', {value: 'Bob'})
//
// This will execute a `debugger;` statement when myobj['name'] is defined or
// set to 'Bob'.
function traced(obj, traced_prop, check) {
  return new Proxy(obj, PropertyTracer(traced_prop, check));
}
