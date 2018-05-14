/**
 * Tests the resetError logic when the bulk api enforces the write concern for unordered
 * writes. The tests indirectly checks whether resetError was called by inspecting the
 * response of the getLastError command after executing the bulk ops.
 *
 */

(function() {
    "use strict";
    const coll = db.bulk_legacy_enforce_gle;

    /**
     * Inserts 'doc' into the collection, asserting that the write succeeds. This runs a
     * getLastError if the insert does not return a response.
     */
    function insertDocument(doc) {
        let res = coll.insert(doc);
        if (res) {
            assert.writeOK(res);
        } else {
            assert.gleOK(db.runCommand({getLastError: 1}));
        }
    }

    coll.drop();
    let bulk = coll.initializeUnorderedBulkOp();
    bulk.find({none: 1}).upsert().updateOne({_id: 1});
    assert.writeOK(bulk.execute());
    let gle = assert.gleOK(db.runCommand({getLastError: 1}));
    assert.eq(1, gle.n, tojson(gle));

    // Batch of size 1 should not call resetError even when it errors out.
    assert(coll.drop());
    insertDocument({_id: 1});
    bulk = coll.initializeUnorderedBulkOp();
    bulk.find({none: 1}).upsert().updateOne({_id: 1});
    assert.throws(function() {
        bulk.execute();
    });

    gle = db.runCommand({getLastError: 1});
    assert(gle.ok, tojson(gle));
    assert.neq(null, gle.err, tojson(gle));

    // Batch with all error except last should not call resetError.
    assert(coll.drop());
    insertDocument({_id: 1});
    bulk = coll.initializeUnorderedBulkOp();
    bulk.find({none: 1}).upsert().updateOne({_id: 1});
    bulk.find({none: 1}).upsert().updateOne({_id: 1});
    bulk.find({none: 1}).upsert().updateOne({_id: 0});
    let res = assert.throws(function() {
        bulk.execute();
    });
    assert.eq(2, res.getWriteErrors().length);

    gle = db.runCommand({getLastError: 1});
    assert(gle.ok, tojson(gle));
    assert.eq(1, gle.n, tojson(gle));

    // Batch with error at middle should not call resetError.
    assert(coll.drop());
    insertDocument({_id: 1});
    bulk = coll.initializeUnorderedBulkOp();
    bulk.find({none: 1}).upsert().updateOne({_id: 0});
    bulk.find({none: 1}).upsert().updateOne({_id: 1});
    bulk.find({none: 1}).upsert().updateOne({_id: 2});
    res = assert.throws(function() {
        bulk.execute();
    });
    assert.eq(1, res.getWriteErrors().length);

    gle = db.runCommand({getLastError: 1});
    assert(gle.ok, tojson(gle));
    // For legacy writes, mongos sends the bulk as one while the shell sends the write individually.
    assert.gte(gle.n, 1, tojson(gle));

    // Batch with error at last should call resetError.
    assert(coll.drop());
    insertDocument({_id: 2});
    bulk = coll.initializeUnorderedBulkOp();
    bulk.find({none: 1}).upsert().updateOne({_id: 0});
    bulk.find({none: 1}).upsert().updateOne({_id: 1});
    bulk.find({none: 1}).upsert().updateOne({_id: 2});
    res = assert.throws(function() {
        bulk.execute();
    });
    assert.eq(1, res.getWriteErrors().length);

    gle = db.runCommand({getLastError: 1});
    assert(gle.ok, tojson(gle));
    assert.eq(0, gle.n, tojson(gle));

    // Batch with error at last should not call resetError if { w: 1 }.
    assert(coll.drop());
    insertDocument({_id: 2});
    bulk = coll.initializeUnorderedBulkOp();
    bulk.find({none: 1}).upsert().updateOne({_id: 0});
    bulk.find({none: 1}).upsert().updateOne({_id: 1});
    bulk.find({none: 1}).upsert().updateOne({_id: 2});
    res = assert.throws(function() {
        bulk.execute();
    });
    assert.eq(1, res.getWriteErrors().length);

    gle = db.runCommand({getLastError: 1, w: 1});
    assert(gle.ok, tojson(gle));
    assert.neq(null, gle.err, tojson(gle));

    // Batch with error at last should not call resetError if { w: 0 }.
    assert(coll.drop());
    insertDocument({_id: 2});
    bulk = coll.initializeUnorderedBulkOp();
    bulk.find({none: 1}).upsert().updateOne({_id: 0});
    bulk.find({none: 1}).upsert().updateOne({_id: 1});
    bulk.find({none: 1}).upsert().updateOne({_id: 2});
    res = assert.throws(function() {
        bulk.execute();
    });
    assert.eq(1, res.getWriteErrors().length, () => tojson(res));

    gle = db.runCommand({getLastError: 1, w: 0});
    assert(gle.ok, tojson(gle));
    assert.neq(null, gle.err, tojson(gle));
}());
