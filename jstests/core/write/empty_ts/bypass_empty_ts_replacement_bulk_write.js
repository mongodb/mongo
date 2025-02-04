/**
 * Tests the "bypassEmptyTsReplacement" option used with the bulkWrite command.
 *
 * @tags: [
 *   not_allowed_with_signed_security_token,
 *   requires_fcv_81,
 * ]
 */

const coll = db.jstests_bypass_empty_ts_replacement_bulk_write;
const dbName = db.getName();
const collName = coll.getName();
const emptyTs = Timestamp(0, 0);

coll.drop();

let numCallsToRunTests = 0;

function runTests(bypassEmptyTsReplacement) {
    ++numCallsToRunTests;

    let first = (numCallsToRunTests == 1);
    let startId = numCallsToRunTests * 100;

    function getId(i) {
        return startId + i;
    }

    // The bulkWirte commands below will be run with the bypassEmptyTsReplacement parameter set to
    // true or false (depending whether the 'bypassEmptyTsReplacement' variable is true or false).

    // Use the bulkWrite command to insert several documents.
    assert.commandWorked(db.adminCommand({
        bulkWrite: 1,
        ops: [
            {insert: 0, document: {_id: getId(1), a: emptyTs}},
            {insert: 0, document: {_id: getId(2), a: 1}},
            {insert: 0, document: {_id: getId(4), a: 2}},
            {insert: 0, document: {_id: getId(5), a: 3}},
            {insert: 0, document: {_id: getId(6), a: 4}}
        ],
        nsInfo: [{ns: dbName + "." + collName}],
        bypassEmptyTsReplacement: bypassEmptyTsReplacement
    }));

    // Use the bulkWrite command to update several documents.
    assert.commandWorked(db.adminCommand({
        bulkWrite: 1,
        ops: [
            // Use a replacement-style update to update the doc with _id=getId(2).
            {update: 0, filter: {_id: getId(2)}, updateMods: {a: emptyTs}},

            // Do a replacement-style update to add a new document with _id=getId(3).
            {update: 0, filter: {_id: getId(3)}, updateMods: {a: emptyTs}, upsert: true},

            // Do an update-operator-style update to update the doc with _id=getId(4).
            {update: 0, filter: {_id: getId(4)}, updateMods: {$set: {a: emptyTs}}},

            // Do a pipeline-style update to update the doc with _id=getId(5).
            {update: 0, filter: {_id: getId(5)}, updateMods: [{$addFields: {a: emptyTs}}]},

            // Do a pipeline-style update with $internalApplyOplogUpdate to update the doc with
            // _id=getId(6).
            {
                update: 0,
                filter: {_id: getId(6)},
                updateMods:
                    [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}]
            },

            // Do an update-operator-style update to add a new document with _id=getId(7).
            {update: 0, filter: {_id: getId(7)}, updateMods: {$set: {a: emptyTs}}, upsert: true},

            // Do a pipline-style update to add a new document with _id=getId(8).
            {
                update: 0,
                filter: {_id: getId(8)},
                updateMods: [{$addFields: {a: emptyTs}}],
                upsert: true
            },

            // Do a pipeline-style update with $internalApplyOplogUpdate to add a new document
            // with _id=getId(9).
            {
                update: 0,
                filter: {_id: getId(9)},
                updateMods:
                    [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}],
                upsert: true
            }
        ],
        nsInfo: [{ns: dbName + "." + collName}],
        bypassEmptyTsReplacement: bypassEmptyTsReplacement
    }));

    // Verify that the bulkWrite commands behaved the way we expected. If 'bypassEmptyTsReplacement'
    // is true, then we expect field "a" will be equal to the empty timestamp for all 9 documents.
    // If 'bypassEmptyTsReplacement' is false, then we expect field "a" will be equal to the current
    // timestamp for the first 3 documents and it will be equal to the empty timestamp for the other
    // 6 documents.
    for (let i = 1; i <= 9; ++i) {
        let result = coll.findOne({_id: getId(i)});

        if (bypassEmptyTsReplacement || i >= 4) {
            assert.eq(tojson(result.a), tojson(emptyTs), "_id=" + getId(i));
        } else {
            assert.neq(tojson(result.a), tojson(emptyTs), "_id=" + getId(i));
        }
    }

    if (first) {
        // If this is the first time that runTests() is invoked, use the bulkWrite command to
        // insert a document with _id=Timestamp(0,0), and then verify that the document can be
        // retrieved using the filter "{_id: Timestamp(0,0)}".
        assert.commandWorked(db.adminCommand({
            bulkWrite: 1,
            ops: [{insert: 0, document: {_id: emptyTs, a: 3}}],
            nsInfo: [{ns: dbName + "." + collName}],
            bypassEmptyTsReplacement: bypassEmptyTsReplacement
        }));

        let result = coll.findOne({_id: emptyTs});
        assert.eq(tojson(result._id), tojson(emptyTs), "_id=" + tojson(emptyTs));
        assert.eq(tojson(result.a), tojson(3), "_id=" + tojson(emptyTs));
    }

    // Use the bulkWrite command to do a replacement-style update on the document.
    assert.commandWorked(db.adminCommand({
        bulkWrite: 1,
        ops: [{update: 0, filter: {_id: emptyTs}, updateMods: {_id: emptyTs, a: emptyTs}}],
        nsInfo: [{ns: dbName + "." + collName}],
        bypassEmptyTsReplacement: bypassEmptyTsReplacement
    }));

    // Verify the document we just updated can still be retrieved using "{_id: Timestamp(0,0)}".
    let result = coll.findOne({_id: emptyTs});
    assert.eq(tojson(result._id), tojson(emptyTs), "_id=" + tojson(emptyTs));

    if (bypassEmptyTsReplacement) {
        // Verify that an empty timestamp value was stored in field "a".
        assert.eq(tojson(result.a), tojson(emptyTs), "_id=" + tojson(emptyTs));
    } else {
        // Verify that field "a" was set to the current timestamp.
        assert.neq(tojson(result.a), tojson(emptyTs), "_id=" + tojson(emptyTs));
    }
}

let bypassEmptyTsReplacement = true;
runTests(bypassEmptyTsReplacement);

bypassEmptyTsReplacement = false;
runTests(bypassEmptyTsReplacement);
