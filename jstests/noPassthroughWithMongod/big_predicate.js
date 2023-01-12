/**
 * Test that queries containing an AND or an OR with a lot of clauses can be answered.
 */
(function() {
"use strict";

const coll = db.big_predicate;
coll.drop();

let filter = {};
for (let i = 0; i < 2500; ++i) {
    filter["field" + i] = i;
}

assert.commandWorked(coll.insert({foo: 1}));
assert.commandWorked(coll.insert(filter));

assert.eq(coll.find(filter).itcount(), 1);
assert.commandWorked(coll.explain().find(filter).finish());

assert.eq(coll.find({$or: [filter]}).itcount(), 1);
assert.commandWorked(coll.explain().find({$or: [filter]}).finish());
assert.commandWorked(coll.explain().find({$and: [filter]}).finish());
})();

/**
 * Test that a query containing many nested $ands and $ors can succeed, or if past the nesting
 * limit, triggers the appropriate error.
 */
(function() {
"use strict";

const coll = db.big_predicate;
coll.drop();

function buildQuery(maxDepth, currentDepth = 0) {
    if (currentDepth >= maxDepth - 1) {
        return {e: 1};
    }
    return {$and: [{$or: [buildQuery(maxDepth, currentDepth + 4), {o: 0}]}, {a: 1}]};
}

// At the documented maximum nesting depth of 100, all commands are guaranteed to succeed.
let filter = buildQuery(100);
assert.commandWorked(coll.insert({e: 1}));
assert.eq(coll.find(filter).itcount(), 0);
assert.commandWorked(coll.explain().find(filter).finish());

// As a buffer past the official limit, queries succeed even at a nesting depth of 130.
filter = buildQuery(130);
assert.eq(coll.find(filter).itcount(), 0);
assert.commandWorked(coll.explain().find(filter).finish());

// Attempting a query that is too deeply nested results in an "exceeded depth limit" error.
filter = buildQuery(150);
assert.throwsWithCode(() => coll.find(filter).itcount(), 17279);
assert.throwsWithCode(() => coll.explain().find(filter).finish(), 17279);
})();

/**
 * Test that a query containing a long path can succeed, or if past the path length limit, triggers
 * the appropriate error.
 */
(function() {
"use strict";

const coll = db.big_predicate;
coll.drop();

function buildObj(maxDepth, currentDepth = 0) {
    if (currentDepth >= maxDepth - 1) {
        return {a: 1};
    }
    return {a: buildObj(maxDepth, currentDepth + 1)};
}

function testDepth(depth) {
    let jobj = buildObj(depth);
    jobj.foo = 1;
    assert.commandWorked(coll.insert(jobj));
    assert.eq(coll.find({foo: 1}).itcount(), 1);

    let filterOnLongField = {
        ['a' +
         '.a'.repeat(depth - 1)]: 1
    };

    assert.eq(coll.find(filterOnLongField).itcount(), 1);
    assert.commandWorked(coll.explain().find(filterOnLongField).finish());

    assert.eq(coll.find({foo: 1}, filterOnLongField).itcount(), 1);
    assert.commandWorked(coll.explain().find({foo: 1}, filterOnLongField).finish());

    let sliceProjectionOnLongField = {
        ['a' +
         '.a'.repeat(depth - 1)]: {$slice: 1}
    };

    assert.eq(coll.find({foo: 1}, sliceProjectionOnLongField).itcount(), 1);
    assert.commandWorked(coll.explain().find({foo: 1}, sliceProjectionOnLongField).finish());

    coll.drop();
}

// At the documented maximum nesting depth of 100, all commands are guaranteed to succeed.
testDepth(100);

// This is a stress test. We test the absolute maximum nesting depth, beyond what is officially
// supported, to ensure the server doesn't crash on any of these operations, because in the past
// some of these operations have caused a segfault.
function testMaxDepth() {
    let depth = 100;
    let jobj = buildObj(depth);
    let result;
    jobj.foo = 1;
    assert.commandWorked(result = coll.insert(jobj));
    assert.eq(coll.find({foo: 1}).itcount(), 1);

    for (result; !result.hasWriteError(); depth++) {
        result = coll.update({foo: 1}, {
            $set: {
                ['a' +
                 '.a'.repeat(depth - 1)]: {a: 1},
            }
        });
    }
    assert.eq(result.getWriteError().code, ErrorCodes.Overflow);
    print("Failed at creating a document with a depth of " + depth + ".");
    depth--;

    let fieldDepth = depth;

    // When ASAN is on, filtering on a long field exceeds the stack limit, causing a segfault.
    if (_isAddressSanitizerActive()) {
        fieldDepth = depth / 2;
        jsTestLog("Lowering the maximum depth from " + depth + " to " + fieldDepth +
                  " because the address sanitizer is active.");
        assert.commandWorked(coll.update({foo: 1}, {
            $set: {
                ['a' +
                 '.a'.repeat(fieldDepth - 1)]: 1,
            }
        }));
    }

    let filterOnLongField = {
        ['a' +
         '.a'.repeat(fieldDepth - 1)]: 1
    };

    assert.eq(coll.find(filterOnLongField).itcount(), 1);
    assert.commandWorked(coll.explain().find(filterOnLongField).finish());

    assert.eq(coll.find({foo: 1}, filterOnLongField).itcount(), 1);
    assert.commandWorked(coll.explain().find({foo: 1}, filterOnLongField).finish());

    let sliceProjectionOnLongField = {
        ['a' +
         '.a'.repeat(fieldDepth - 1)]: {$slice: 1}
    };

    assert.eq(coll.find({foo: 1}, sliceProjectionOnLongField).itcount(), 1);
    assert.commandWorked(coll.explain().find({foo: 1}, sliceProjectionOnLongField).finish());

    coll.drop();
}
// Filter, projection, and $slice succeed even at the internal maximum nesting depth of 180.
testMaxDepth();

function testDepthThatShouldFail(depth) {
    let jobj = buildObj(depth);

    jobj.foo = 1;
    assert.commandWorked(coll.insert({foo: 1}));
    assert.throwsWithCode(() => coll.insert(jobj), 17279);
    assert.eq(coll.find({foo: 1}).itcount(), 1);

    let filterOnLongField = {
        ['a' +
         '.a'.repeat(depth - 1)]: 1
    };

    assert.throwsWithCode(() => coll.find(filterOnLongField).itcount(), 5729100);
    assert.throwsWithCode(() => coll.explain().find(filterOnLongField).finish(), 5729100);

    assert.throwsWithCode(() => coll.find({foo: 1}, filterOnLongField).itcount(),
                          ErrorCodes.Overflow);
    assert.throwsWithCode(() => coll.explain().find({foo: 1}, filterOnLongField).finish(),
                          ErrorCodes.Overflow);

    let sliceProjectionOnLongField = {
        ['a' +
         '.a'.repeat(depth - 1)]: {$slice: 1}
    };

    assert.throwsWithCode(() => coll.find({foo: 1}, sliceProjectionOnLongField).itcount(),
                          ErrorCodes.Overflow);
    assert.throwsWithCode(() => coll.explain().find({foo: 1}, sliceProjectionOnLongField).finish(),
                          ErrorCodes.Overflow);

    coll.drop();
}

// A field that is too long results in a "FieldPath is too long" error.
testDepthThatShouldFail(201);
})();
