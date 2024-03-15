/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

"use strict";

loadRelativeToScript('utility.js');
loadRelativeToScript('annotations.js');

var options = parse_options([
    { name: "gcTypes", default: "gcTypes.txt" },
    { name: "typeInfo", default: "typeInfo.txt" }
]);

var typeInfo = {
    'GCPointers': [],
    'GCThings': [],
    'GCInvalidated': [],
    'GCRefs': [],
    'NonGCTypes': {}, // unused
    'NonGCPointers': {},
    'RootedGCThings': {},
    'RootedPointers': {},
    'RootedBases': {'JS::AutoGCRooter': true},
    'InheritFromTemplateArgs': {},
    'OtherCSUTags': {},
    'OtherFieldTags': {},

    // RAII types within which we should assume GC is suppressed, eg
    // AutoSuppressGC.
    'GCSuppressors': {},
};

var gDescriptors = new Map; // Map from descriptor string => Set of typeName

var structureParents = {}; // Map from field => list of <parent, fieldName>
var pointerParents = {}; // Map from field => list of <parent, fieldName>
var baseClasses = {}; // Map from struct name => list of base class name strings
var subClasses = {}; // Map from struct name => list of subclass  name strings

var gcTypes = {}; // map from parent struct => Set of GC typed children
var gcPointers = {}; // map from parent struct => Set of GC typed children
var gcFields = new Map;

var rootedPointers = {};

// Accumulate the base GC types before propagating info through the type graph,
// so that we can include edges from types processed later
// (eg MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS).
var pendingGCTypes = []; // array of [name, reason, ptrdness]

function processCSU(csu, body)
{
    for (let { 'Name': [ annType, tag ] } of (body.Annotation || [])) {
        if (annType != 'annotate')
            continue;

        if (tag == 'GC Pointer')
            typeInfo.GCPointers.push(csu);
        else if (tag == 'Invalidated by GC')
            typeInfo.GCInvalidated.push(csu);
        else if (tag == 'GC Pointer or Reference')
            typeInfo.GCRefs.push(csu);
        else if (tag == 'GC Thing')
            typeInfo.GCThings.push(csu);
        else if (tag == 'Suppressed GC Pointer')
            typeInfo.NonGCPointers[csu] = true;
        else if (tag == 'Rooted Pointer')
            typeInfo.RootedPointers[csu] = true;
        else if (tag == 'Rooted Base')
            typeInfo.RootedBases[csu] = true;
        else if (tag == 'Suppress GC')
            typeInfo.GCSuppressors[csu] = true;
        else if (tag == 'moz_inherit_type_annotations_from_template_args')
            typeInfo.InheritFromTemplateArgs[csu] = true;
        else
            addToKeyedList(typeInfo.OtherCSUTags, csu, tag);
    }

    for (let { 'Base': base } of (body.CSUBaseClass || []))
        addBaseClass(csu, base);

    for (const field of (body.DataField || [])) {
        var type = field.Field.Type;
        var fieldName = field.Field.Name[0];
        if (type.Kind == "Pointer") {
            var target = type.Type;
            if (target.Kind == "CSU")
                addNestedPointer(csu, target.Name, fieldName);
        }
        if (type.Kind == "Array") {
            var target = type.Type;
            if (target.Kind == "CSU")
                addNestedStructure(csu, target.Name, fieldName);
        }
        if (type.Kind == "CSU")
            addNestedStructure(csu, type.Name, fieldName);

        for (const { 'Name': [ annType, tag ] } of (field.Annotation || [])) {
            if (!(csu in typeInfo.OtherFieldTags))
                typeInfo.OtherFieldTags[csu] = [];
            addToKeyedList(typeInfo.OtherFieldTags[csu], fieldName, tag);
        }
    }

    for (const funcfield of (body.FunctionField || [])) {
        const fields = funcfield.Field;
        // Pure virtual functions will not have field.Variable; others will.
        for (const field of funcfield.Field) {
            for (const {'Name': [annType, tag]} of (field.Annotation || [])) {
                if (!(csu in typeInfo.OtherFieldTags))
                    typeInfo.OtherFieldTags[csu] = {};
                addToKeyedList(typeInfo.OtherFieldTags[csu], field.Name[0], tag);
            }
        }
    }
}

// csu.field is of type inner
function addNestedStructure(csu, inner, field)
{
    if (!(inner in structureParents))
        structureParents[inner] = [];

    // Skip fields that are really base classes, to avoid duplicating the base
    // fields; addBaseClass already added a "base-N" name.
    if (field.match(/^field:\d+$/) && (csu in baseClasses) && (baseClasses[csu].indexOf(inner) != -1))
        return;

    structureParents[inner].push([ csu, field ]);
}

function addBaseClass(csu, base) {
    if (!(csu in baseClasses))
        baseClasses[csu] = [];
    baseClasses[csu].push(base);
    if (!(base in subClasses))
        subClasses[base] = [];
    subClasses[base].push(csu);
    var k = baseClasses[csu].length;
    addNestedStructure(csu, base, `<base-${k}>`);
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

for (const typename of extraRootedGCThings())
    typeInfo.RootedGCThings[typename] = true;

for (const typename of extraRootedPointers())
    typeInfo.RootedPointers[typename] = true;

// Everything that inherits from a "Rooted Base" is considered to be rooted.
// This is for things like CustomAutoRooter and its subclasses.
var basework = Object.keys(typeInfo.RootedBases);
while (basework.length) {
    const base = basework.pop();
    typeInfo.RootedPointers[base] = true;
    if (base in subClasses)
        basework.push(...subClasses[base]);
}

// Now that we have the whole hierarchy set up, add all the types and propagate
// info.
for (const csu of typeInfo.GCThings)
    addGCType(csu);
for (const csu of typeInfo.GCPointers)
    addGCPointer(csu);
for (const csu of typeInfo.GCInvalidated)
    addGCPointer(csu);

function parseTemplateType(typeName, validate=false) {
    // We only want templatized types. `Foo<U, T>::Member` doesn't count.
    // Foo<U, T>::Bar<X, Y> does count. Which turns out to be a simple rule:
    // check whether the type ends in '>'.
    if (!typeName.endsWith(">")) {
        return [typeName, undefined];
    }

    // "Tokenize" into angle brackets, commas, and everything else. We store
    // match objects as tokens because we'll need the string offset after we
    // finish grabbing the template parameters.
    const tokens = [];
    const tokenizer = /[<>,]|[^<>,]+/g;
    let match;
    while ((match = tokenizer.exec(typeName)) !== null) {
    	tokens.push(match);
    }

    // Walk backwards through the tokens, stopping when we find the matching
    // open bracket.
    const args = [];
    let depth = 0;
    let arg;
    let first_result;
    for (const match of tokens.reverse()) {
        const token = match[0];
        if (depth == 1 && (token == ',' || token == '<')) {
            // We've walked back to the beginning of a template parameter,
            // where we will see either a comma or open bracket.
            args.unshift(arg);
            arg = '';
        } else if (depth == 0 && token == '>') {
            arg = ''; // We just started.
        } else {
            arg = token + arg;
        }

        // Maintain the depth.
        if (token == '<') {
            // This could be bug 1728151.
            assert(depth > 0, `Invalid type: too many '<' signs in '${typeName}'`);
            depth--;
        } else if (token == '>') {
            depth++;
        }

        if (depth == 0) {
            // We've walked out of the template parameter list.
            // Record the results.
            assert(args.length > 0);
            const templateName = typeName.substr(0, match.index);
            const result = [templateName, args.map(arg => arg.trim())];
            if (!validate) {
                // Normal processing is to return the result the first time we
                // get to the '<' that matches the terminal '>', without validating
                // that the rest of the type name is balanced.
                return result;
            } else if (!first_result) {
                // If we are validating, remember the result when we hit the
                // first matching '<', but then keep processing the rest of
                // the input string to count brackets.
                first_result = result;
            }
        }
    }

    // This could be bug 1728151.
    assert(depth == 0, `Invalid type: too many '>' signs in '${typeName}'`);
    return first_result;
}

if (os.getenv("HAZARD_RUN_INTERNAL_TESTS")) {
    function check_parse(typeName, result) {
        assertEq(JSON.stringify(parseTemplateType(typeName)), JSON.stringify(result));
    }

    check_parse("int", ["int", undefined]);
    check_parse("Type<int>", ["Type", ["int"]]);
    check_parse("Container<int, double>", ["Container", ["int", "double"]]);
    check_parse("Container<Container<void, void>, double>", ["Container", ["Container<void, void>", "double"]]);
    check_parse("Foo<Bar<a,b>,Bar<a,b>>::Container<Container<void, void>, double>", ["Foo<Bar<a,b>,Bar<a,b>>::Container", ["Container<void, void>", "double"]]);
    check_parse("AlignedStorage2<TypedArray<foo>>", ["AlignedStorage2", ["TypedArray<foo>"]]);
    check_parse("mozilla::AlignedStorage2<mozilla::dom::TypedArray<unsigned char, JS::UnwrapArrayBufferMaybeShared, JS::GetArrayBufferMaybeSharedData, JS::GetArrayBufferMaybeSharedLengthAndData, JS::NewArrayBuffer> >",
        [
            "mozilla::AlignedStorage2",
            [
                "mozilla::dom::TypedArray<unsigned char, JS::UnwrapArrayBufferMaybeShared, JS::GetArrayBufferMaybeSharedData, JS::GetArrayBufferMaybeSharedLengthAndData, JS::NewArrayBuffer>"
            ]
        ]
    );
    check_parse(
        "mozilla::ArrayIterator<const mozilla::dom::binding_detail::RecordEntry<nsTString<char16_t>, mozilla::dom::Nullable<mozilla::dom::TypedArray<unsigned char, JS::UnwrapArrayBufferMaybeShared, JS::GetArrayBufferMaybeSharedData, JS::GetArrayBufferMaybeSharedLengthAndData, JS::NewArrayBuffer> > >&, nsTArray_Impl<mozilla::dom::binding_detail::RecordEntry<nsTString<char16_t>, mozilla::dom::Nullable<mozilla::dom::TypedArray<unsigned char, JS::UnwrapArrayBufferMaybeShared, JS::GetArrayBufferMaybeSharedData, JS::GetArrayBufferMaybeSharedLengthAndData, JS::NewArrayBuffer> > >, nsTArrayInfallibleAllocator> >",
        [
            "mozilla::ArrayIterator",
            [
                "const mozilla::dom::binding_detail::RecordEntry<nsTString<char16_t>, mozilla::dom::Nullable<mozilla::dom::TypedArray<unsigned char, JS::UnwrapArrayBufferMaybeShared, JS::GetArrayBufferMaybeSharedData, JS::GetArrayBufferMaybeSharedLengthAndData, JS::NewArrayBuffer> > >&",
                "nsTArray_Impl<mozilla::dom::binding_detail::RecordEntry<nsTString<char16_t>, mozilla::dom::Nullable<mozilla::dom::TypedArray<unsigned char, JS::UnwrapArrayBufferMaybeShared, JS::GetArrayBufferMaybeSharedData, JS::GetArrayBufferMaybeSharedLengthAndData, JS::NewArrayBuffer> > >, nsTArrayInfallibleAllocator>"
            ]
        ]
    );

    function check_throws(f, exc) {
        try {
            f();
        } catch (e) {
            assertEq(e.message.includes(exc), true, "incorrect exception: " + e.message);
            return;
        }
        assertEq(undefined, exc);
    }
    // Note that these need to end in '>' or the whole thing will be ignored.
    check_throws(() => parseTemplateType("foo>", true), "too many '>' signs");
    check_throws(() => parseTemplateType("foo<<>", true), "too many '<' signs");
    check_throws(() => parseTemplateType("foo<a::bar<a,b>", true), "too many '<' signs");
    check_throws(() => parseTemplateType("foo<a>*>::bar<a,b>", true), "too many '>' signs");
}

// GC Thing and GC Pointer annotations can be inherited from template args if
// this annotation is used. Think of Maybe<T> for example: Maybe<JSObject*> has
// the same GC rules as JSObject*.

var inheritors = Object.keys(typeInfo.InheritFromTemplateArgs).sort((a, b) => a.length - b.length);
for (const csu of inheritors) {
    const [templateName, templateArgs] = parseTemplateType(csu);
    for (const param of templateArgs) {
        const pos = param.search(/\**$/);
        const ptrdness = param.length - pos;
        const core_type = param.substr(0, pos);
        if (ptrdness == 0) {
            addToKeyedList(structureParents, core_type, [csu, "template-param-" + param]);
        } else if (ptrdness == 1) {
            addToKeyedList(pointerParents, core_type, [csu, "template-param-" + param]);
        }
    }
}

// "typeName is a (pointer to a)^'typePtrLevel' GC type because it contains a field
// named 'child' of type 'why' (or pointer to 'why' if fieldPtrLevel == 1), which is
// itself a GCThing or GCPointer."
function markGCType(typeName, child, why, typePtrLevel, fieldPtrLevel, indent)
{
    // Some types, like UniquePtr, do not mark/trace/relocate their contained
    // pointers and so should not hold them live across a GC. UniquePtr in
    // particular should be the only thing pointing to a structure containing a
    // GCPointer, so nothing else can possibly trace it and it'll die when the
    // UniquePtr goes out of scope. So we say that memory pointed to by a
    // UniquePtr is just as unsafe as the stack for storing GC pointers.
    if (!fieldPtrLevel && isUnsafeStorage(typeName)) {
        // The UniquePtr itself is on the stack but when you dereference the
        // contained pointer, you get to the unsafe memory that we are treating
        // as if it were the stack (aka ptrLevel 0). Note that
        // UniquePtr<UniquePtr<JSObject*>> is fine, so we don't want to just
        // hardcode the ptrLevel.
        fieldPtrLevel = -1;
    }

    // Example: with:
    //    struct Pair { JSObject* foo; int bar; };
    //    struct { Pair** info }***
    // make a call to:
    //    child='info' typePtrLevel=3 fieldPtrLevel=2
    // for a final ptrLevel of 5, used to later call:
    //    child='foo' typePtrLevel=5 fieldPtrLevel=1
    //
    var ptrLevel = typePtrLevel + fieldPtrLevel;

    // ...except when > 2 levels of pointers away from an actual GC thing, stop
    // searching the graph. (This would just be > 1, except that a UniquePtr
    // field might still have a GC pointer.)
    if (ptrLevel > 2)
        return;

    if (isRootedGCPointerTypeName(typeName) && !(typeName in typeInfo.RootedPointers))
        printErr("FIXME: use in-source annotation for " + typeName);

    if (ptrLevel == 0 && (typeName in typeInfo.RootedGCThings))
        return;
    if (ptrLevel == 1 && (isRootedGCPointerTypeName(typeName) || (typeName in typeInfo.RootedPointers)))
        return;

    if (ptrLevel == 0) {
        if (typeName in typeInfo.NonGCTypes)
            return;
        if (!(typeName in gcTypes))
            gcTypes[typeName] = new Set();
        gcTypes[typeName].add(why);
    } else if (ptrLevel == 1) {
        if (typeName in typeInfo.NonGCPointers)
            return;
        if (!(typeName in gcPointers))
            gcPointers[typeName] = new Set();
        gcPointers[typeName].add(why);
    }

    if (ptrLevel < 2) {
        if (!gcFields.has(typeName))
            gcFields.set(typeName, new Map());
        gcFields.get(typeName).set(child, [ why, fieldPtrLevel ]);
    }

    if (typeName in structureParents) {
        for (var field of structureParents[typeName]) {
            var [ holderType, fieldName ] = field;
            markGCType(holderType, fieldName, typeName, ptrLevel, 0, indent + "  ");
        }
    }
    if (typeName in pointerParents) {
        for (var field of pointerParents[typeName]) {
            var [ holderType, fieldName ] = field;
            markGCType(holderType, fieldName, typeName, ptrLevel, 1, indent + "  ");
        }
    }
}

function addGCType(typeName, child, why, depth, fieldPtrLevel)
{
    pendingGCTypes.push([typeName, '<annotation>', '(annotation)', 0, 0]);
}

function addGCPointer(typeName)
{
    pendingGCTypes.push([typeName, '<pointer-annotation>', '(annotation)', 1, 0]);
}

for (const pending of pendingGCTypes) {
    markGCType(...pending);
}

// Call a function for a type and every type that contains the type in a field
// or as a base class (which internally is pretty much the same thing --
// subclasses are structs beginning with the base class and adding on their
// local fields.)
function foreachContainingStruct(typeName, func, seen = new Set())
{
    function recurse(container, typeName) {
        if (seen.has(typeName))
            return;
        seen.add(typeName);

        func(container, typeName);

        if (typeName in subClasses) {
            for (const sub of subClasses[typeName])
                recurse("subclass of " + typeName, sub);
        }
        if (typeName in structureParents) {
            for (const [holder, field] of structureParents[typeName])
                recurse(field + " : " + typeName, holder);
        }
    }

    recurse('<annotation>', typeName);
}

for (var type of listNonGCPointers())
    typeInfo.NonGCPointers[type] = true;

function explain(csu, indent, seen) {
    if (!seen)
        seen = new Set();
    seen.add(csu);
    if (!gcFields.has(csu))
        return;
    var fields = gcFields.get(csu);

    if (fields.has('<annotation>')) {
        print(indent + "which is annotated as a GCThing");
        return;
    }
    if (fields.has('<pointer-annotation>')) {
        print(indent + "which is annotated as a GCPointer");
        return;
    }
    for (var [ field, [ child, ptrdness ] ] of fields) {
        var msg = indent;
        if (field[0] == '<')
            msg += "inherits from ";
        else {
            if (field.startsWith("template-param-")) {
                msg += "inherits annotations from template parameter '" + field.substr(15) + "' ";
            } else {
                msg += "contains field '" + field + "' ";
            }
            if (ptrdness == -1)
                msg += "(with a pointer to unsafe storage) holding a ";
            else if (ptrdness == 0)
                msg += "of type ";
            else
                msg += "pointing to type ";
        }
        msg += child;
        print(msg);
        if (!seen.has(child))
            explain(child, indent + "  ", seen);
    }
}

var origOut = os.file.redirect(options.gcTypes);

for (var csu in gcTypes) {
    print("GCThing: " + csu);
    explain(csu, "  ");
}
for (var csu in gcPointers) {
    print("GCPointer: " + csu);
    explain(csu, "  ");
}

// Redirect output to the typeInfo file and close the gcTypes file.
os.file.close(os.file.redirect(options.typeInfo));

// Compute the set of types that suppress GC within their RAII scopes (eg
// AutoSuppressGC, AutoSuppressGCForAnalysis).
var seen = new Set();
for (let csu in typeInfo.GCSuppressors)
    foreachContainingStruct(csu,
                            (holder, typeName) => { typeInfo.GCSuppressors[typeName] = holder },
                            seen);

print(JSON.stringify(typeInfo, null, 4));

os.file.close(os.file.redirect(origOut));
