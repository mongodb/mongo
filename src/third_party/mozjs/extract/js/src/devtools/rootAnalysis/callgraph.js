/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

loadRelativeToScript('utility.js');
loadRelativeToScript('annotations.js');
loadRelativeToScript('CFG.js');

// Map from csu => set of immediate subclasses
var subclasses = new Map();

// Map from csu => set of immediate superclasses
var superclasses = new Map();

// Map from "csu.name:nargs" => set of full method name
var virtualDefinitions = new Map();

// Every virtual method declaration, anywhere.
//
//   field : CFG of the field
var virtualResolutionsSeen = new Set();

// map is a map from names to sets of entries.
function addToNamedSet(map, name, entry)
{
    if (!map.has(name))
        map.set(name, new Set());
    map.get(name).add(entry);
}

function fieldKey(csuName, field)
{
    // This makes a minimal attempt at dealing with overloading: it will not
    // conflate two virtual methods with differing numbers of arguments. So
    // far, that is all that has been needed.
    var nargs = 0;
    if (field.Type.Kind == "Function" && "TypeFunctionArguments" in field.Type)
        nargs = field.Type.TypeFunctionArguments.Type.length;
    return csuName + ":" + field.Name[0] + ":" + nargs;
}

// CSU is "Class/Struct/Union"
function processCSU(csuName, csu)
{
    if (!("FunctionField" in csu))
        return;
    for (const field of csu.FunctionField) {
        if (1 in field.Field) {
            const superclass = field.Field[1].Type.Name;
            const subclass = field.Field[1].FieldCSU.Type.Name;
            assert(subclass == csuName);
            addToNamedSet(subclasses, superclass, subclass);
            addToNamedSet(superclasses, subclass, superclass);
        }

        if ("Variable" in field) {
            // Note: not dealing with overloading correctly.
            const name = field.Variable.Name[0];
            addToNamedSet(virtualDefinitions, fieldKey(csuName, field.Field[0]), name);
        }
    }
}

// Return the nearest ancestor method definition, or all nearest definitions in
// the case of multiple inheritance.
function nearestAncestorMethods(csu, field)
{
    const key = fieldKey(csu, field);

    if (virtualDefinitions.has(key))
        return new Set(virtualDefinitions.get(key));

    const functions = new Set();
    if (superclasses.has(csu)) {
        for (const parent of superclasses.get(csu))
            functions.update(nearestAncestorMethods(parent, field));
    }

    return functions;
}

// Return [ instantiations, limits ], where instantiations is a Set of all
// possible implementations of 'field' given static type 'initialCSU', plus
// null if arbitrary other implementations are possible, and limits gives
// information about what things are not possible within it (currently, that it
// cannot GC).
function findVirtualFunctions(initialCSU, field)
{
    const fieldName = field.Name[0];
    const worklist = [initialCSU];
    const functions = new Set();

    // Loop through all methods of initialCSU (by looking at all methods of ancestor csus).
    //
    // If field is nsISupports::AddRef or ::Release, return an empty list and a
    // boolean that says we assert that it cannot GC.
    //
    // If this is a method that is annotated to be dangerous (eg, it could be
    // overridden with an implementation that could GC), then use null as a
    // signal value that it should be considered to GC, even though we'll also
    // collect all of the instantiations for other purposes.

    while (worklist.length) {
        const csu = worklist.pop();
        if (isSuppressedVirtualMethod(csu, fieldName))
            return [ new Set(), LIMIT_CANNOT_GC ];
        if (isOverridableField(initialCSU, csu, fieldName)) {
            // We will still resolve the virtual function call, because it's
            // nice to have as complete a callgraph as possible for other uses.
            // But push a token saying that we can run arbitrary code.
            functions.add(null);
        }

        if (superclasses.has(csu))
            worklist.push(...superclasses.get(csu));
    }

    // Now return a list of all the instantiations of the method named 'field'
    // that could execute on an instance of initialCSU or a descendant class.

    // Start with the class itself, or if it doesn't define the method, all
    // nearest ancestor definitions.
    functions.update(nearestAncestorMethods(initialCSU, field));

    // Then recurse through all descendants to add in their definitions.

    worklist.push(initialCSU);
    while (worklist.length) {
        const csu = worklist.pop();
        const key = fieldKey(csu, field);

        if (virtualDefinitions.has(key))
            functions.update(virtualDefinitions.get(key));

        if (subclasses.has(csu))
            worklist.push(...subclasses.get(csu));
    }

    return [ functions, LIMIT_NONE ];
}

// Return a list of all callees that the given edge might be a call to. Each
// one is represented by an object with a 'kind' field that is one of
// ('direct', 'field', 'resolved-field', 'indirect', 'unknown'), though note
// that 'resolved-field' is really a global record of virtual method
// resolutions, indepedent of this particular edge.
function getCallees(edge)
{
    if (edge.Kind != "Call")
        return [];

    const callee = edge.Exp[0];
    if (callee.Kind == "Var") {
        assert(callee.Variable.Kind == "Func");
        return [{'kind': 'direct', 'name': callee.Variable.Name[0]}];
    }

    if (callee.Kind == "Int")
        return []; // Intentional crash

    assert(callee.Kind == "Drf");
    const called = callee.Exp[0];
    if (called.Kind == "Var") {
        // indirect call through a variable.
        return [{'kind': "indirect", 'variable': callee.Exp[0].Variable.Name[0]}];
    }

    if (called.Kind != "Fld") {
        // unknown call target.
        return [{'kind': "unknown"}];
    }

    const callees = [];
    const field = callee.Exp[0].Field;
    const fieldName = field.Name[0];
    const csuName = field.FieldCSU.Type.Name;
    let functions;
    let limits = LIMIT_NONE;
    if ("FieldInstanceFunction" in field) {
        [ functions, limits ] = findVirtualFunctions(csuName, field);
        callees.push({'kind': "field", 'csu': csuName, 'field': fieldName,
                      'limits': limits, 'isVirtual': true});
    } else {
        functions = new Set([null]); // field call
    }

    // Known set of virtual call targets. Treat them as direct calls to all
    // possible resolved types, but also record edges from this field call to
    // each final callee. When the analysis is checking whether an edge can GC
    // and it sees an unrooted pointer held live across this field call, it
    // will know whether any of the direct callees can GC or not.
    const targets = [];
    let fullyResolved = true;
    for (const name of functions) {
        if (name === null) {
            // Unknown set of call targets, meaning either a function pointer
            // call ("field call") or a virtual method that can be overridden
            // in extensions. Use the isVirtual property so that callers can
            // tell which case holds.
            callees.push({'kind': "field", 'csu': csuName, 'field': fieldName,
                          'limits': limits,
			  'isVirtual': "FieldInstanceFunction" in field});
            fullyResolved = false;
        } else {
            targets.push({'kind': "direct", name, limits });
        }
    }
    if (fullyResolved)
        callees.push({'kind': "resolved-field", 'csu': csuName, 'field': fieldName, 'callees': targets});

    return callees;
}

function loadTypes(type_xdb_filename) {
    const xdb = xdbLibrary();
    xdb.open(type_xdb_filename);

    const minStream = xdb.min_data_stream();
    const maxStream = xdb.max_data_stream();

    for (var csuIndex = minStream; csuIndex <= maxStream; csuIndex++) {
        const csu = xdb.read_key(csuIndex);
        const data = xdb.read_entry(csu);
        const json = JSON.parse(data.readString());
        processCSU(csu.readString(), json[0]);

        xdb.free_string(csu);
        xdb.free_string(data);
    }
}

function loadTypesWithCache(type_xdb_filename, cache_filename) {
    try {
        const cacheAB = os.file.readFile(cache_filename, "binary");
        const cb = serialize();
        cb.clonebuffer = cacheAB.buffer;
        const cacheData = deserialize(cb);
        subclasses = cacheData.subclasses;
        superclasses = cacheData.superclasses;
        virtualDefinitions = cacheData.virtualDefinitions;
    } catch (e) {
        loadTypes(type_xdb_filename);
        const cb = serialize({subclasses, superclasses, virtualDefinitions});
        os.file.writeTypedArrayToFile(cache_filename,
                                      new Uint8Array(cb.arraybuffer));
    }
}
