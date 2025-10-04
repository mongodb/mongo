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
// Map from csu => Set of function-info.
// function-info: {
//   name : simple string
//   typedfield : "name:nargs" ("mangled" field name)
//   field: full Field datastructure
//   annotations : Set of [annotation-name, annotation-value] 2-element arrays
//   inherited : whether the method is inherited from a base class
//   pureVirtual : whether the method is pure virtual on this CSU
//   dtor : if this is a virtual destructor with a definition in this class or
//     a superclass, then the full name of the definition as if it were defined
//     in this class. This is weird, but it's how gcc emits it. We will add a
//     synthetic call from this function to its immediate base classes' dtors,
//     so even if the function does not actually exist and is inherited from a
//     base class, we will get a path to the inherited function. (Regular
//     virtual methods are *not* claimed to exist when they don't.)
// }
var virtualDeclarations = new Map();

var virtualResolutionsSeen = new Set();

var ID = {
    jscode: 1,
    anyfunc: 2,
    nogcfunc: 3,
    gc: 4,
};

// map is a map from names to sets of entries.
function addToNamedSet(map, name, entry)
{
    if (!map.has(name))
      map.set(name, new Set());
    const s = map.get(name);
    s.add(entry);
    return s;
}

// CSU is "Class/Struct/Union"
function processCSU(csuName, csu)
{
    if (!("FunctionField" in csu))
        return;

    for (const {Base} of (csu.CSUBaseClass || [])) {
        addToNamedSet(subclasses, Base, csuName);
        addToNamedSet(superclasses, csuName, Base);
    }

    for (const {Field, Variable} of csu.FunctionField) {
        // Virtual method
        const info = Field[0];
        const name = info.Name[0];
        const annotations = new Set();
        const funcInfo = {
            name,
            typedfield: typedField(info),
            field: info,
            annotations,
            inherited: (info.FieldCSU.Type.Name != csuName), // Always false for virtual dtors
            pureVirtual: Boolean(Variable),
            dtor: false,
        };

        if (Variable && isSyntheticVirtualDestructor(name)) {
            // This is one of gcc's artificial dtors.
            funcInfo.dtor = Variable.Name[0];
            funcInfo.pureVirtual = false;
        }

        addToNamedSet(virtualDeclarations, csuName, funcInfo);
        if ('Annotation' in info) {
            for (const {Name: [annType, annValue]} of info.Annotation) {
                annotations.add([annType, annValue]);
            }
        }

        if (Variable) {
            // Note: not dealing with overloading correctly.
            const name = Variable.Name[0];
            addToNamedSet(virtualDefinitions, fieldKey(csuName, Field[0]), name);
        }
    }
}

// Return a list of all callees that the given edge might be a call to. Each
// one is represented by an object with a 'kind' field that is one of
// ('direct', 'field', 'resolved-field', 'indirect', 'unknown'), though note
// that 'resolved-field' is really a global record of virtual method
// resolutions, indepedent of this particular edge.
function translateCallees(edge)
{
    if (edge.Kind != "Call")
        return [];

    const callee = edge.Exp[0];
    if (callee.Kind == "Var") {
        assert(callee.Variable.Kind == "Func");
        return [{'kind': 'direct', 'name': callee.Variable.Name[0]}];
    }

    // At some point, we were intentionally invoking invalid function pointers
    // (as in, a small integer cast to a function pointer type) to convey a
    // small amount of information in the crash address.
    if (callee.Kind == "Int")
        return []; // Intentional crash

    assert(callee.Kind == "Drf");
    let called = callee.Exp[0];
    let indirection = 1;
    if (called.Kind == "Drf") {
        // This is probably a reference to a function pointer (`func*&`). It
        // would be possible to determine that for certain by looking up the
        // variable's type, which is doable but unnecessary. Indirect calls
        // are assumed to call anything (any function in the codebase) unless they
        // are annotated otherwise, and the `funkyName` annotation applies to
        // `(**funkyName)(args)` as well as `(*funkyName)(args)`, it's ok.
        called = called.Exp[0];
        indirection += 1;
    }

    if (called.Kind == "Var") {
        // indirect call through a variable. Note that the `indirection` field is
        // currently unused by the later analysis. It is the number of dereferences
        // applied to the variable before invoking the resulting function.
        //
        // The variable name passed through is the simplified one, since that is
        // what annotations.js uses and we don't want the annotation to be missed
        // if eg there is another variable of the same name in a sibling scope such
        // that the fully decorated name no longer matches.
        const [decorated, bare] = called.Variable.Name;
        return [{'kind': "indirect", 'variable': bare, indirection}];
    }

    if (called.Kind != "Fld") {
        // unknown call target.
        return [{'kind': "unknown"}];
    }

    // Return one 'field' callee record giving the full description of what's
    // happening here (which is either a virtual method call, or a call through
    // a function pointer stored in a field), and then boil the call down to a
    // synthetic function that incorporates both the name of the field and the
    // static type of whatever you're calling the method on. Both refer to the
    // same call; they're just different ways of describing it.
    const callees = [];
    const field = called.Field;
    const staticCSU = getFieldCallInstanceCSU(edge, field);
    callees.push({'kind': "field", 'csu': field.FieldCSU.Type.Name, staticCSU,
                  'field': field.Name[0], 'fieldKey': fieldKey(staticCSU, field),
                  'isVirtual': ("FieldInstanceFunction" in field)});
    callees.push({'kind': "direct", 'name': fieldKey(staticCSU, field)});

    return callees;
}

function getCallees(body, edge, scopeAttrs, functionBodies) {
    const calls = [];

    // getCallEdgeProperties can set the ATTR_REPLACED attribute, which
    // means that the call in the edge has been replaced by zero or
    // more edges to other functions. This is used when the original
    // edge will end up calling through a function pointer or something
    // (eg ~shared_ptr<T> calls a function pointer that can only be
    // T::~T()). The original call edges are left in the graph in case
    // they are useful for other purposes.
    for (const callee of translateCallees(edge)) {
        if (callee.kind != "direct") {
            calls.push({ callee, attrs: scopeAttrs });
        } else {
            const edgeInfo = getCallEdgeProperties(body, edge, callee.name, functionBodies);
            for (const extra of (edgeInfo.extraCalls || [])) {
                calls.push({ attrs: scopeAttrs | extra.attrs, callee: { name: extra.name, 'kind': "direct", } });
            }
            calls.push({ callee, attrs: scopeAttrs | edgeInfo.attrs});
        }
    }

    return calls;
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
