/**
 * Test how the bulkWrite command behaves with "Timestamp(0,0)" values.
 *
 * @tags: [
 *   not_allowed_with_signed_security_token,
 *   requires_fcv_81,
 * ]
 */

const coll = db.jstests_core_empty_ts_bulk_write;
const dbName = db.getName();
const collName = coll.getName();
const emptyTs = Timestamp(0, 0);

coll.drop();

// Use the bulkWrite command to insert several documents.
assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 101, a: emptyTs}},
        {insert: 0, document: {_id: 102, a: 1}},
        {insert: 0, document: {_id: 104, a: 2}},
        {insert: 0, document: {_id: 105, a: 3}},
        {insert: 0, document: {_id: 106, a: 4}}
    ],
    nsInfo: [{ns: dbName + "." + collName}]
}));

// Use the bulkWrite command to update several documents.
assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [
        // Use a replacement-style update to update _id=102. This should result in field "a" being
        // set to the current timestamp.
        {update: 0, filter: {_id: 102}, updateMods: {a: emptyTs}},

        // Do a replacement-style update to add a new document with _id=103. This should result in
        // field "a" being set to the current timestamp.
        {update: 0, filter: {_id: 103}, updateMods: {a: emptyTs}, upsert: true},

        // For the rest of the operations below, the empty timestamp values stored in field "a"
        // should be preserved as-is.

        // Do an update-operator-style update to update _id=104.
        {update: 0, filter: {_id: 104}, updateMods: {$set: {a: emptyTs}}},

        // Do a pipeline-style update to update _id=105.
        {update: 0, filter: {_id: 105}, updateMods: [{$addFields: {a: emptyTs}}]},

        // Do a pipeline-style update with $internalApplyOplogUpdate to update _id=106.
        {
            update: 0,
            filter: {_id: 106},
            updateMods:
                [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}]
        },

        // Do an update-operator-style update to add a new document with _id=107.
        {update: 0, filter: {_id: 107}, updateMods: {$set: {a: emptyTs}}, upsert: true},

        // Do a pipline-style update to add a new document with _id=108.
        {update: 0, filter: {_id: 108}, updateMods: [{$addFields: {a: emptyTs}}], upsert: true},

        // Do a pipeline-style update with $internalApplyOplogUpdate to add a new document _id=109.
        {
            update: 0,
            filter: {_id: 109},
            updateMods:
                [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}],
            upsert: true
        }
    ],
    nsInfo: [{ns: dbName + "." + collName}]
}));

// Verify that all the bulkWrite commands behaved the way we expected.
for (let i = 101; i <= 109; ++i) {
    let result = coll.findOne({_id: i});

    if (i >= 104) {
        assert.eq(tojson(result.a), tojson(emptyTs), "_id=" + i);
    } else {
        assert.neq(tojson(result.a), tojson(emptyTs), "_id=" + i);
    }
}

// Use the bulkWrite command to insert a document with _id=Timestamp(0,0).
assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {_id: emptyTs, a: 5}}],
    nsInfo: [{ns: dbName + "." + collName}]
}));

// Verify the document we just inserted can be retrieved using the filter "{_id: Timestamp(0,0)}".
let result = coll.findOne({_id: emptyTs});
assert.eq(tojson(result._id), tojson(emptyTs), "_id=" + tojson(emptyTs));
assert.eq(tojson(result.a), tojson(5), "_id=" + tojson(emptyTs));

// Use the bulkWrite command to do a replacement-style update on the document.
assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [{update: 0, filter: {_id: emptyTs}, updateMods: {_id: emptyTs, a: emptyTs}}],
    nsInfo: [{ns: dbName + "." + collName}]
}));

// Verify the document we just updated can still be retrieved using "{_id: Timestamp(0,0)}", and
// verify that field "a" was set to the current timestamp.
result = coll.findOne({_id: emptyTs});
assert.eq(tojson(result._id), tojson(emptyTs), "_id=" + tojson(emptyTs));
assert.neq(tojson(result.a), tojson(emptyTs), "_id=" + tojson(emptyTs));
