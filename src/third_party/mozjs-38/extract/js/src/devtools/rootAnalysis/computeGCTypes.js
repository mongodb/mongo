/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

"use strict";

loadRelativeToScript('utility.js');
loadRelativeToScript('annotations.js');

function processCSU(csu, body)
{
    if (!("DataField" in body))
        return;
    for (var field of body.DataField) {
        var type = field.Field.Type;
        var fieldName = field.Field.Name[0];
        if (type.Kind == "Pointer") {
            var target = type.Type;
            if (target.Kind == "CSU")
                addNestedPointer(csu, target.Name, fieldName);
        }
        if (type.Kind == "CSU") {
            // Ignore nesting in classes which are AutoGCRooters. We only consider
            // types with fields that may not be properly rooted.
            if (type.Name == "JS::AutoGCRooter" || type.Name == "JS::CustomAutoRooter")
                return;
            addNestedStructure(csu, type.Name, fieldName);
        }
    }
}

var structureParents = {}; // Map from field => list of <parent, fieldName>
var pointerParents = {}; // Map from field => list of <parent, fieldName>

function addNestedStructure(csu, inner, field)
{
    if (!(inner in structureParents))
        structureParents[inner] = [];
    structureParents[inner].push([ csu, field ]);
}

function addNestedPointer(csu, inner, field)
{
    if (!(inner in pointerParents))
        pointerParents[inner] = [];
    pointerParents[inner].push([ csu, field ]);
}

var xdb = xdbLibrary();
xdb.open("src_comp.xdb");

var minStream = xdb.min_data_stream();
var maxStream = xdb.max_data_stream();

for (var csuIndex = minStream; csuIndex <= maxStream; csuIndex++) {
    var csu = xdb.read_key(csuIndex);
    var data = xdb.read_entry(csu);
    var json = JSON.parse(data.readString());
    assert(json.length == 1);
    processCSU(csu.readString(), json[0]);

    xdb.free_string(csu);
    xdb.free_string(data);
}

var gcTypes = {}; // map from parent struct => Set of GC typed children
var gcPointers = {}; // map from parent struct => Set of GC typed children
var gcFields = {};

// "typeName is a (pointer to a)*'depth' GC type because it contains a field
// named 'child' of type 'why' (or pointer to 'why' if ptrdness == 1), which
// itself a GCThing or GCPointer."
function markGCType(typeName, child, why, depth, ptrdness)
{
    // Some types, like UniquePtr, do not mark/trace/relocate their contained
    // pointers and so should not hold them live across a GC. UniquePtr in
    // particular should be the only thing pointing to a structure containing a
    // GCPointer, so nothing else can be tracing it and it'll die when the
    // UniquePtr goes out of scope. So we say that a UniquePtr's memory is just
    // as unsafe as the stack for storing GC pointers.
    if (!ptrdness && isUnsafeStorage(typeName)) {
        printErr("Unsafe! " + typeName);
        // The UniquePtr itself is on the stack but when you dereference the
        // contained pointer, you get to the unsafe memory that we are treating
        // as if it were the stack (aka depth 0). Note that
        // UniquePtr<UniquePtr<JSObject*>> is fine, so we don't want to just
        // hardcode the depth.
        ptrdness = -1;
    }

    depth += ptrdness;
    if (depth > 2)
        return;

    if (depth == 0 && isRootedTypeName(typeName))
        return;
    if (depth == 1 && isRootedPointerTypeName(typeName))
        return;

    if (depth == 0) {
        if (!(typeName in gcTypes))
            gcTypes[typeName] = new Set();
        gcTypes[typeName].add(why);
    } else if (depth == 1) {
        if (!(typeName in gcPointers))
            gcPointers[typeName] = new Set();
        gcPointers[typeName].add(why);
    }

    if (!(typeName in gcFields))
        gcFields[typeName] = new Map();
    gcFields[typeName].set(why, [ child, ptrdness ]);

    if (typeName in structureParents) {
        for (var field of structureParents[typeName]) {
            var [ holderType, fieldName ] = field;
            markGCType(holderType, typeName, fieldName, depth, 0);
        }
    }
    if (typeName in pointerParents) {
        for (var field of pointerParents[typeName]) {
            var [ holderType, fieldName ] = field;
            markGCType(holderType, typeName, fieldName, depth, 1);
        }
    }
}

function addGCType(typeName, child, why, depth, ptrdness)
{
    markGCType(typeName, 'annotation', '<annotation>', 0, 0);
}

function addGCPointer(typeName)
{
    markGCType(typeName, 'annotation', '<pointer-annotation>', 1, 0);
}

addGCType('JSObject');
addGCType('JSString');
addGCType('js::Shape');
addGCType('js::BaseShape');
addGCType('JSScript');
addGCType('js::LazyScript');
addGCType('js::ion::IonCode');
addGCPointer('JS::Value');
addGCPointer('jsid');

// AutoCheckCannotGC should also not be held live across a GC function.
addGCPointer('JS::AutoCheckCannotGC');

function explain(csu, indent, seen) {
    if (!seen)
        seen = new Set();
    seen.add(csu);
    if (!(csu in gcFields))
        return;
    if (gcFields[csu].has('<annotation>')) {
        print(indent + "which is a GCThing because I said so");
        return;
    }
    if (gcFields[csu].has('<pointer-annotation>')) {
        print(indent + "which is a GCPointer because I said so");
        return;
    }
    for (var [ field, [ child, ptrdness ] ] of gcFields[csu]) {
        var inherit = "";
        if (field == "field:0")
            inherit = " (probably via inheritance)";
        var msg = indent + "contains field '" + field + "' ";
        if (ptrdness == -1)
            msg += "(with a pointer to unsafe storage) holding a ";
        else if (ptrdness == 0)
            msg += "of type ";
        else
            msg += "pointing to type ";
        msg += child + inherit;
        print(msg);
        if (!seen.has(child))
            explain(child, indent + "  ", seen);
    }
}

for (var csu in gcTypes) {
    print("GCThing: " + csu);
    explain(csu, "  ");
}
for (var csu in gcPointers) {
    print("GCPointer: " + csu);
    explain(csu, "  ");
}
