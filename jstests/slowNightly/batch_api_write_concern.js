/**
 * Fail Due to no journal and requesting journal write concern
 */
var m = MongoRunner.runMongod({ "nojournal" : "" });
var db = m.getDB("test");

var coll = db.getCollection("batch_api_write_concern");

jsTest.log("Starting batch api write concern tests...");

coll.remove({});

var request;
var result;

/********************************************************
 *
 * Ordered tests should return same results for write command as
 * well as for the legacy operations
 *
 *******************************************************/
var executeOrderedTests = function() {
    // Remove collection
    coll.dropIndexes();
    coll.remove({});

    //
    // Fail due to no journal enabled
    var batch = coll.initializeOrderedBulkOp();
    batch.insert({a:1});
    batch.insert({a:2});
    var error = false;
    var result = null;

    // Should throw error
    try {
        result = batch.execute({j:true});
    } catch(err) {
        error = true;
    }

    assert(error);

    //
    // Fail due to write concern support
    var batch = coll.initializeOrderedBulkOp();
    batch.insert({a:1});
    batch.insert({a:2});
    var error = false;

    try {
        result = batch.execute({w:2, wtimeout:1000});
    } catch(err) {
        error = true;
    }

    assert(error);

    // Create unique index
    coll.remove({});
    coll.ensureIndex({a : 1}, {unique : true});

    /**
    // XXX TEST INVALID UNTIL SERVER-12274 is fixed for shell

    //
    // Fail with write concern error and duplicate key error
    var batch = coll.initializeOrderedBulkOp();
    batch.insert({a:1});
    batch.insert({a:1});
    batch.insert({a:2});
    var error = false;

    try {
        result = batch.execute({w:2, wtimeout:1000});
    } catch(err) {
        error = true;
    }

    assert(error);
    */

    // Remove collection
    coll.dropIndexes();
    coll.remove({});
}

/********************************************************
 *
 * UnOrdered tests should return same results for write command as
 * well as for the legacy operations
 *
 *******************************************************/
var executeUnorderedTests = function() {
    // Create unique index
    coll.remove({});
    coll.ensureIndex({a : 1}, {unique : true});

    //
    // Fail due to write concern support
    var batch = coll.initializeUnorderedBulkOp();
    batch.insert({a:1});
    batch.find({a:3}).upsert().updateOne({a:3, b:1})
    batch.insert({a:2});
    var result = batch.execute({w:5, wtimeout:1});
    assert.eq(2, result.nInserted);
    assert.eq(1, result.nUpserted);
    assert.eq(1, result.getUpsertedIds().length);
    assert.eq(1, result.getUpsertedIdAt(0).index);
    assert.eq(3, coll.count());
    assert.eq(64, result.getWriteConcernError().code);
    assert(result.getWriteConcernError().errmsg != null);

    // Create unique index
    coll.remove({});
    coll.ensureIndex({a : 1}, {unique : true});

    //
    // Fail due to write concern support as well as duplicate key error on unordered batch
    var batch = coll.initializeUnorderedBulkOp();
    batch.insert({a:1});
    batch.find({a:3}).upsert().updateOne({a:3, b:1})
    batch.insert({a:1})
    batch.insert({a:2});
    var result = batch.execute({w:5, wtimeout:1});
    assert.eq(2, result.nInserted);
    assert.eq(1, result.nUpserted);
    assert.eq(1, result.getUpsertedIds().length);
    assert.eq(1, result.getUpsertedIdAt(0).index);
    assert.eq(3, coll.count());
    assert.eq(64, result.getWriteConcernError().code);
    assert(result.getWriteConcernError().errmsg != null);

    // Create unique index
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
    executeOrderedTests();
    executeUnorderedTests();
}

// Force the use of legacy commands
coll._mongo.useWriteCommands = function() {
    return false;
}

// Execute tests using legacy operations
executeOrderedTests();
executeUnorderedTests();

// Reset the function
coll.getMongo().useWriteCommands = _useWriteCommands;