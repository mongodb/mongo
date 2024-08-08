/**
 * Tests the "bypassEmptyTsReplacement" option.
 *
 * @tags: [
 *   requires_fcv_50,
 * ]
 */
(function() {
"use strict";

const coll = db.jstests_bypass_empty_ts_replacement;
const collName = coll.getName();
const emptyTs = Timestamp(0, 0);

coll.drop();

function doInsert(docs, bypassEmptyTsReplacement) {
    let cmdRes = db.runCommand(
        {insert: collName, documents: docs, bypassEmptyTsReplacement: bypassEmptyTsReplacement});
    assert.commandWorked(cmdRes);
}

function doUpdate(filter, update, bypassEmptyTsReplacement) {
    let cmdRes = db.runCommand({
        update: collName,
        updates: [{q: filter, u: update}],
        bypassEmptyTsReplacement: bypassEmptyTsReplacement
    });
    assert.commandWorked(cmdRes);
}

function doUpdateWithUpsert(filter, update, bypassEmptyTsReplacement) {
    let cmdRes = db.runCommand({
        update: collName,
        updates: [{q: filter, u: update, upsert: true}],
        bypassEmptyTsReplacement: bypassEmptyTsReplacement
    });
    assert.commandWorked(cmdRes);
}

function doFindAndModify(filter, update, bypassEmptyTsReplacement) {
    let cmdRes = db.runCommand({
        findAndModify: collName,
        query: filter,
        update: update,
        bypassEmptyTsReplacement: bypassEmptyTsReplacement
    });
    assert.commandWorked(cmdRes);
}

function doFindAndModifyWithUpsert(filter, update, bypassEmptyTsReplacement) {
    let cmdRes = db.runCommand({
        findAndModify: collName,
        query: filter,
        update: update,
        upsert: true,
        bypassEmptyTsReplacement: bypassEmptyTsReplacement
    });
    assert.commandWorked(cmdRes);
}

let numCallsToRunTests = 0;

function runTests(bypassEmptyTsReplacement) {
    ++numCallsToRunTests;

    let first = (numCallsToRunTests == 1);
    let startId = numCallsToRunTests * 100;

    function getId(i) {
        return startId + i;
    }

    // The insert/update/findAndModify commands below will be run with the bypassEmptyTsReplacement
    // parameter set to true or false (depending whether the 'bypassEmptyTsReplacement' variable is
    // true or false).

    // Insert several documents.
    doInsert([{_id: getId(1), a: emptyTs}], bypassEmptyTsReplacement);
    doInsert([{_id: getId(2), a: 1}], bypassEmptyTsReplacement);
    doInsert([{_id: getId(3), a: 2}], bypassEmptyTsReplacement);
    doInsert([{_id: getId(6), a: 3}], bypassEmptyTsReplacement);
    doInsert([{_id: getId(7), a: 4}], bypassEmptyTsReplacement);
    doInsert([{_id: getId(8), a: 5}], bypassEmptyTsReplacement);
    doInsert([{_id: getId(9), a: 6}], bypassEmptyTsReplacement);
    doInsert([{_id: getId(10), a: 7}], bypassEmptyTsReplacement);
    doInsert([{_id: getId(11), a: 8}], bypassEmptyTsReplacement);

    // Use a replacement-style update to update the doc with _id=getId(2).
    //
    // When bypassEmptyTsReplacement=true, this is an example of how the special "empty timestamp"
    // behavior can be suppressed when doing a replacement-style update.
    doUpdate({_id: getId(2)}, {a: emptyTs}, bypassEmptyTsReplacement);

    // Use a replacement-style findAndModify to update the doc with _id=getId(3).
    doFindAndModify({_id: getId(3)}, {a: emptyTs}, bypassEmptyTsReplacement);

    // Do a replacement-style update to add a new document with _id=getId(4).
    doUpdateWithUpsert({_id: getId(4)}, {a: emptyTs}, bypassEmptyTsReplacement);

    // Do a replacement-style findAndModify to add a new document with _id=getId(5).
    doFindAndModifyWithUpsert({_id: getId(5)}, {a: emptyTs}, bypassEmptyTsReplacement);

    // Do an update-operator-style update to update the doc with _id=getId(6).
    doUpdate({_id: getId(6)}, {$set: {a: emptyTs}}, bypassEmptyTsReplacement);

    // Do an update-operator-style findAndModify to update the doc with _id=getId(7).
    doFindAndModify({_id: getId(7)}, {$set: {a: emptyTs}}, bypassEmptyTsReplacement);

    // Do a pipeline-style update to update the doc with _id=getId(8).
    doUpdate({_id: getId(8)}, [{$addFields: {a: emptyTs}}], bypassEmptyTsReplacement);

    // Do a pipeline-style findAndModify to update the doc with _id=getId(9).
    doFindAndModify({_id: getId(9)}, [{$addFields: {a: emptyTs}}], bypassEmptyTsReplacement);

    // Do a pipeline-style update with $internalApplyOplogUpdate to update the doc with
    // _id=getId(10).
    doUpdate({_id: getId(10)},
             [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}],
             bypassEmptyTsReplacement);

    // Do a pipeline-style findAndModify with $internalApplyOplogUpdate to update the doc with
    // _id=getId(11).
    doFindAndModify({_id: getId(11)},
                    [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}],
                    bypassEmptyTsReplacement);

    // Do an update-operator-style update to add a new document with _id=getId(12).
    doUpdateWithUpsert({_id: getId(12)}, {$set: {a: emptyTs}}, bypassEmptyTsReplacement);

    // Do an update-operator-style findAndModify to add a new document with _id=getId(13).
    doFindAndModifyWithUpsert({_id: getId(13)}, {$set: {a: emptyTs}}, bypassEmptyTsReplacement);

    // Do a pipeline-style update to add a new document with _id=getId(14).
    doUpdateWithUpsert({_id: getId(14)}, [{$addFields: {a: emptyTs}}], bypassEmptyTsReplacement);

    // Do a pipeline-style findAndModify to add a new document with _id=getId(15).
    doFindAndModifyWithUpsert(
        {_id: getId(15)}, [{$addFields: {a: emptyTs}}], bypassEmptyTsReplacement);

    // Do a pipline-style update with $internalApplyOplogUpdate to add a new document with
    // _id=getId(16).
    doUpdateWithUpsert(
        {_id: getId(16)},
        [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}],
        bypassEmptyTsReplacement);

    // Do pipeline-style findAndModify with $internalApplyOplogUpdate to add a new document
    // with _id=getId(17).
    doFindAndModifyWithUpsert(
        {_id: getId(17)},
        [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}],
        bypassEmptyTsReplacement);

    // Verify that all the insert, update, and findAndModify commands behaved the way we expected.
    // If 'bypassEmptyTsReplacement' is true, then we expect field "a" will be equal to the empty
    // timestamp for all 17 documents. If 'bypassEmptyTsReplacement' is false, then we expect
    // field "a" will be equal to the current timestamp for the first 5 documents and it will
    // be equal to the empty timestamp for the other 12 documents.
    for (let i = 1; i <= 17; ++i) {
        let result = coll.findOne({_id: getId(i)});

        if (bypassEmptyTsReplacement || i >= 6) {
            assert.eq(tojson(result.a), tojson(emptyTs), "_id=" + getId(i));
        } else {
            assert.neq(tojson(result.a), tojson(emptyTs), "_id=" + getId(i));
        }
    }

    if (first) {
        // If this is the first time that runTests() is invoked, insert a document with
        // _id=Timestamp(0,0), and then verify that the document we just inserted can be
        // retrieved using the filter "{_id: Timestamp(0,0)}".
        doInsert([{_id: emptyTs, a: 9}], bypassEmptyTsReplacement);

        let result = coll.findOne({_id: emptyTs});
        assert.eq(tojson(result._id), tojson(emptyTs), "_id=" + tojson(emptyTs));
        assert.eq(tojson(result.a), tojson(9), "_id=" + tojson(emptyTs));
    }

    // Do a replacement-style update on the document with _id=Timestamp(0,0).
    doUpdate({_id: emptyTs}, {_id: emptyTs, a: emptyTs}, bypassEmptyTsReplacement);

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
}());
