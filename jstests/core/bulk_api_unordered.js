var collectionName = "bulk_api_unordered";
var coll = db.getCollection(collectionName);
coll.drop();

var request;
var result;

jsTest.log("Starting bulk api unordered tests...");

/********************************************************
 *
 * Unordered tests should return same results for write command as
 * well as for the legacy operations
 *
 *******************************************************/
var executeTests = function() {
    // Remove collection
    coll.remove({});

    /**
     * Single successful unordered batch operation
     */
    var batch = coll.initializeUnorderedBulkOp();
    batch.insert({a:1});
    batch.find({a:1}).updateOne({$set: {b:1}});
    // no-op, should increment nUpdate, but not nModified
    batch.find({a:1}).updateOne({$set: {b:1}});
    batch.find({a:2}).upsert().updateOne({$set: {b:2}});
    batch.insert({a:3});
    batch.find({a:3}).remove({a:3});
    var result = batch.execute();
    assert.eq(2, result.nInserted);
    assert.eq(1, result.nUpserted);
    assert.eq(2, result.nMatched);
    if (coll.getMongo().useWriteCommands()) {
        assert.eq(1, result.nModified);
    }
    else {
        // Legacy updates does not support nModified.
        assert.eq(0, result.nModified);
    }
    assert.eq(1, result.nRemoved);
    assert(1, result.getWriteErrorCount());
    var upserts = result.getUpsertedIds();
    assert.eq(1, upserts.length);
    assert.eq(3, upserts[0].index);
    assert(upserts[0]._id != null);
    var upsert = result.getUpsertedIdAt(0);
    assert.eq(3, upsert.index);
    assert(upsert._id != null);


    // illegal to try to convert a multi-batch op into a SingleWriteResult
    assert.throws(function() { result.toSingleResult(); } );

    // Test SingleWriteResult
    var singleBatch = coll.initializeUnorderedBulkOp();
    singleBatch.find({a:4}).upsert().updateOne({$set: {b:1}});
    var singleResult = singleBatch.execute().toSingleResult();
    assert(singleResult.getUpsertedId() != null);

    // Create unique index
    coll.remove({});
    coll.ensureIndex({a : 1}, {unique : true});

    /**
     * Single error unordered batch operation
     */
    var batch = coll.initializeUnorderedBulkOp();
    batch.insert({b:1, a:1});
    batch.find({b:2}).upsert().updateOne({$set: {a:1}});
    batch.insert({b:3, a:2});
    var result = batch.execute();
    // Basic properties check
    assert.eq(2, result.nInserted);
    assert.eq(true, result.hasWriteErrors());
    assert(1, result.getWriteErrorCount());

    // Get the first error
    var error = result.getWriteErrorAt(0);
    assert.eq(11000, error.code);
    assert(error.errmsg != null);

    // Get the operation that caused the error
    var op = error.getOperation();
    assert.eq(2, op.q.b);
    assert.eq(1, op.u['$set'].a);
    assert.eq(false, op.multi);
    assert.eq(true, op.upsert);

    // Create unique index
    coll.dropIndexes();
    coll.remove({});
    coll.ensureIndex({a : 1}, {unique : true});

    /**
     * Multiple error unordered batch operation
     */
    var batch = coll.initializeUnorderedBulkOp();
    batch.insert({b:1, a:1});
    batch.find({b:2}).upsert().updateOne({$set: {a:1}});
    batch.find({b:3}).upsert().updateOne({$set: {a:2}});
    batch.find({b:2}).upsert().updateOne({$set: {a:1}});
    batch.insert({b:4, a:3});
    batch.insert({b:5, a:1});
    var result = batch.execute();
    // Basic properties check
    assert.eq(2, result.nInserted);
    assert.eq(1, result.nUpserted);
    assert.eq(true, result.hasWriteErrors());
    assert(3, result.getWriteErrorCount());

    // Individual error checking
    var error = result.getWriteErrorAt(0);
    assert.eq(11000, error.code);
    assert(error.errmsg != null);
    assert.eq(2, error.getOperation().q.b);
    assert.eq(1, error.getOperation().u['$set'].a);
    assert.eq(false, error.getOperation().multi);
    assert.eq(true, error.getOperation().upsert);

    var error = result.getWriteErrorAt(1);
    assert.eq(3, error.index);
    assert.eq(11000, error.code);
    assert(error.errmsg != null);
    assert.eq(2, error.getOperation().q.b);
    assert.eq(1, error.getOperation().u['$set'].a);
    assert.eq(false, error.getOperation().multi);
    assert.eq(true, error.getOperation().upsert);

    var error = result.getWriteErrorAt(2);
    assert.eq(5, error.index);
    assert.eq(11000, error.code);
    assert(error.errmsg != null);
    assert.eq(5, error.getOperation().b);
    assert.eq(1, error.getOperation().a);

    // Create unique index
    coll.dropIndexes();
    coll.remove({});
    coll.ensureIndex({a : 1}, {unique : true});
}

var buildVersion = parseInt(db.runCommand({buildInfo:1}).versionArray.slice(0, 3).join(""), 10);
// Save the existing useWriteCommands function
var _useWriteCommands = coll.getMongo().useWriteCommands;

//
// Only execute write command tests if we have > 2.5.5 otherwise
// execute the down converted version
if(buildVersion >= 255) {
    // Force the use of useWriteCommands
    coll._mongo.useWriteCommands = function() {
        return true;
    }

    // Execute tests using legacy operations
    executeTests();
}

// Force the use of legacy commands
coll._mongo.useWriteCommands = function() {
    return false;
}

// Execute tests using legacy operations
executeTests();

// Reset the function
coll.getMongo().useWriteCommands = _useWriteCommands;
