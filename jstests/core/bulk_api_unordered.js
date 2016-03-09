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
     * find() requires selector and $key is disallowed as field name
     */
    var bulkOp = coll.initializeUnorderedBulkOp();

    assert.throws(function() {
        bulkOp.find();
    });
    assert.throws(function() {
        bulkOp.insert({$key: 1});
    });

    /**
     * Single successful unordered bulk operation
     */
    var bulkOp = coll.initializeUnorderedBulkOp();
    bulkOp.insert({a: 1});
    bulkOp.find({a: 1}).updateOne({$set: {b: 1}});
    // no-op, should increment nMatched but not nModified
    bulkOp.find({a: 1}).updateOne({$set: {b: 1}});
    bulkOp.find({a: 2}).upsert().updateOne({$set: {b: 2}});
    bulkOp.insert({a: 3});
    bulkOp.find({a: 3}).update({$set: {b: 1}});
    bulkOp.find({a: 3}).upsert().update({$set: {b: 2}});
    bulkOp.find({a: 10}).upsert().update({$set: {b: 2}});
    bulkOp.find({a: 2}).replaceOne({a: 11});
    bulkOp.find({a: 11}).removeOne();
    bulkOp.find({a: 3}).remove({a: 3});
    var result = bulkOp.execute();
    assert.eq(2, result.nInserted);
    assert.eq(2, result.nUpserted);
    assert.eq(5, result.nMatched);
    // only check nModified if write commands are enabled
    if (coll.getMongo().writeMode() == "commands") {
        assert.eq(4, result.nModified);
    }
    assert.eq(2, result.nRemoved);
    assert.eq(false, result.hasWriteErrors());
    assert.eq(0, result.getWriteErrorCount());
    var upserts = result.getUpsertedIds();
    assert.eq(2, upserts.length);
    assert.eq(3, upserts[0].index);
    assert(upserts[0]._id != null);
    var upsert = result.getUpsertedIdAt(0);
    assert.eq(3, upsert.index);
    assert(upsert._id != null);
    assert.eq(2, coll.find({}).itcount(), "find should return two documents");

    // illegal to try to convert a multi-op batch into a SingleWriteResult
    assert.throws(function() {
        result.toSingleResult();
    });

    // attempt to re-run bulk
    assert.throws(function() {
        bulkOp.execute();
    });

    // Test SingleWriteResult
    var singleBatch = coll.initializeUnorderedBulkOp();
    singleBatch.find({a: 4}).upsert().updateOne({$set: {b: 1}});
    var singleResult = singleBatch.execute().toSingleResult();
    assert(singleResult.getUpsertedId() != null);

    // Create unique index
    coll.remove({});
    coll.ensureIndex({a: 1}, {unique: true});

    /**
     * Single error unordered bulk operation
     */
    var bulkOp = coll.initializeUnorderedBulkOp();
    bulkOp.insert({b: 1, a: 1});
    bulkOp.find({b: 2}).upsert().updateOne({$set: {a: 1}});
    bulkOp.insert({b: 3, a: 2});
    var result = assert.throws(function() {
        bulkOp.execute();
    });

    // Basic properties check
    assert.eq(2, result.nInserted);
    assert.eq(true, result.hasWriteErrors());
    assert.eq(1, result.getWriteErrorCount());

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
    coll.ensureIndex({a: 1}, {unique: true});

    /**
     * Multiple error unordered bulk operation
     */
    var bulkOp = coll.initializeUnorderedBulkOp();
    bulkOp.insert({b: 1, a: 1});
    bulkOp.find({b: 2}).upsert().updateOne({$set: {a: 1}});
    bulkOp.find({b: 3}).upsert().updateOne({$set: {a: 2}});
    bulkOp.find({b: 2}).upsert().updateOne({$set: {a: 1}});
    bulkOp.insert({b: 4, a: 3});
    bulkOp.insert({b: 5, a: 1});
    var result = assert.throws(function() {
        bulkOp.execute();
    });

    // Basic properties check
    assert.eq(2, result.nInserted);
    assert.eq(1, result.nUpserted);
    assert.eq(true, result.hasWriteErrors());
    assert.eq(3, result.getWriteErrorCount());

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
    coll.ensureIndex({a: 1}, {unique: true});
};

var buildVersion = parseInt(db.runCommand({buildInfo: 1}).versionArray.slice(0, 3).join(""), 10);
// Save the existing useWriteCommands function
var _useWriteCommands = coll.getMongo().useWriteCommands;

//
// Only execute write command tests if we have > 2.5.5 otherwise
// execute the down converted version
if (buildVersion >= 255) {
    // Force the use of useWriteCommands
    coll._mongo.useWriteCommands = function() {
        return true;
    };

    // Execute tests using legacy operations
    executeTests();
}

// Force the use of legacy commands
coll._mongo.useWriteCommands = function() {
    return false;
};

// Execute tests using legacy operations
executeTests();

// Reset the function
coll.getMongo().useWriteCommands = _useWriteCommands;
