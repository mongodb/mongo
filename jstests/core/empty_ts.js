/**
 * Test how inserts and updates behave with "Timestamp(0,0)" values.
 *
 * @tags: [
 *   requires_find_command,
 *   requires_fcv_50,
 * ]
 */
(function() {
"use strict";

const coll = db.jstests_core_empty_ts;
const emptyTs = Timestamp(0, 0);

coll.drop();

// Insert several documents. For the first document inserted (_id=101), the empty timestamp value
// in field "a" should get replaced with the current timestamp.
assert.commandWorked(coll.insert({_id: 101, a: emptyTs}));
assert.commandWorked(coll.insert({_id: 102, a: 1}));
assert.commandWorked(coll.insert({_id: 103, a: 2}));
assert.commandWorked(coll.insert({_id: 106, a: 3}));
assert.commandWorked(coll.insert({_id: 107, a: 4}));
assert.commandWorked(coll.insert({_id: 108, a: 5}));
assert.commandWorked(coll.insert({_id: 109, a: 6}));
assert.commandWorked(coll.insert({_id: 110, a: 7}));
assert.commandWorked(coll.insert({_id: 111, a: 8}));

// Use a replacement-style update to update _id=102. This should result in field "a" being set to
// the current timestamp.
//
// This is an example of how the special "empty timestamp" behavior works with a replacement-style
// update.
assert.commandWorked(coll.update({_id: 102}, {a: emptyTs}));

// Use a replacement-style findAndModify to update _id=103. This should result in field "a" being
// set to the current timestamp.
let findAndModifyResult = coll.findAndModify({query: {_id: 103}, update: {a: emptyTs}});
assert.eq(findAndModifyResult, {_id: 103, a: 2});

// Do a replacement-style update to add a new document with _id=104. This should result in field "a"
// being set to the current timestamp.
assert.commandWorked(coll.update({_id: 104}, {a: emptyTs}, {upsert: true}));

// Do a replacement-style findAndModify to add a new document with _id=105. This should result in
// field "a" being set to the current timestamp.
findAndModifyResult = coll.findAndModify({query: {_id: 105}, update: {a: emptyTs}, upsert: true});
assert.eq(findAndModifyResult, null);

// For the rest of the commands below, the empty timestamp values stored in field "a" should be
// preserved as-is.

// Do an update-operator-style update to update _id=106.
assert.commandWorked(coll.update({_id: 106}, {$set: {a: emptyTs}}));

// Do an update-operator-style findAndModify to update _id=107.
findAndModifyResult = coll.findAndModify({query: {_id: 107}, update: {$set: {a: emptyTs}}});
assert.eq(findAndModifyResult, {_id: 107, a: 4});

// Do a pipeline-style update to update _id=108.
assert.commandWorked(coll.update({_id: 108}, [{$addFields: {a: emptyTs}}]));

// Do a pipeline-style findAndModify to update _id=109.
findAndModifyResult = coll.findAndModify({query: {_id: 109}, update: [{$addFields: {a: emptyTs}}]});
assert.eq(findAndModifyResult, {_id: 109, a: 6});

// Do a pipeline-style update with $internalApplyOplogUpdate to update _id=110.
assert.commandWorked(coll.update(
    {_id: 110}, [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}]));

// Do a pipeline-style findAndModify with $internalApplyOplogUpdate to update _id=111.
findAndModifyResult = coll.findAndModify({
    query: {_id: 111},
    update: [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}]
});
assert.eq(findAndModifyResult, {_id: 111, a: 8});

// Do an update-operator-style update to add a new document with _id=112.
assert.commandWorked(coll.update({_id: 112}, {$set: {a: emptyTs}}, {upsert: true}));

// Do an update-operator-style findAndModify to add a new document with _id=113.
findAndModifyResult =
    coll.findAndModify({query: {_id: 113}, update: {$set: {a: emptyTs}}, upsert: true});
assert.eq(findAndModifyResult, null);

// Do a pipeline-style update to add a new document with _id=114.
assert.commandWorked(coll.update({_id: 114}, [{$addFields: {a: emptyTs}}], {upsert: true}));

// Do a pipeline-style findAndModify to add a new document with _id=115.
findAndModifyResult =
    coll.findAndModify({query: {_id: 115}, update: [{$addFields: {a: emptyTs}}], upsert: true});
assert.eq(findAndModifyResult, null);

// Do a pipline-style update with $internalApplyOplogUpdate to add a new document _id=116.
assert.commandWorked(
    coll.update({_id: 116},
                [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}],
                {upsert: true}));

// Do pipeline-style findAndModify with $internalApplyOplogUpdate to add a new document _id=117.
findAndModifyResult = coll.findAndModify({
    query: {_id: 117},
    update: [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}],
    upsert: true
});
assert.eq(findAndModifyResult, null);

// Verify that all the insert, update, and findAndModify commands behaved the way we expected.
for (let i = 101; i <= 117; ++i) {
    let result = coll.findOne({_id: i});

    if (i >= 106) {
        assert.eq(tojson(result.a), tojson(emptyTs), "_id=" + i);
    } else {
        assert.neq(tojson(result.a), tojson(emptyTs), "_id=" + i);
    }
}

// Insert a document with _id=Timestamp(0,0).
assert.commandWorked(coll.insert({_id: emptyTs, a: 9}));

// Verify the document we just inserted can be retrieved using the filter "{_id: Timestamp(0,0)}".
let result = coll.findOne({_id: emptyTs});
assert.eq(tojson(result._id), tojson(emptyTs), "_id=" + tojson(emptyTs));
assert.eq(tojson(result.a), tojson(9), "_id=" + tojson(emptyTs));

// Do a replacement-style update on the document.
assert.commandWorked(coll.update({_id: emptyTs}, {_id: emptyTs, a: emptyTs}));

// Verify the document we just updated can still be retrieved using "{_id: Timestamp(0,0)}", and
// verify that field "a" was set to the current timestamp.
result = coll.findOne({_id: emptyTs});
assert.eq(tojson(result._id), tojson(emptyTs), "_id=" + tojson(emptyTs));
assert.neq(tojson(result.a), tojson(emptyTs), "_id=" + tojson(emptyTs));
}());
