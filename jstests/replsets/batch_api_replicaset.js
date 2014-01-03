load("jstests/replsets/rslib.js");

var rst = new ReplSetTest({name: 'testSet', nodes: 3});
rst.startSet();
rst.initiate();

var rstConn = new Mongo(rst.getURL());
var coll = rstConn.getCollection("test.batch_write_command_repl");

jsTest.log("Starting batch api replicaset tests...");

/*******************************************************************
 *
 * Ordered
 *
 *******************************************************************/
var executeOrderedTests = function() {
    // Create unique index
    coll.remove({});
    coll.ensureIndex({a : 1}, {unique : true});

    var error = false;
    var result = null;

    //
    // Fail due to write concern support
    var batch = coll.initializeOrderedBulkOp();
    batch.insert({a:1});
    batch.insert({a:2});

    // Should throw error
    try {
        result = batch.execute({w:5, wtimeout:1});
    } catch(err) {
        error = true;
    }

    assert(error);

    // Create unique index
    coll.remove({});
    coll.ensureIndex({a : 1}, {unique : true});

    //
    // Fail due to write concern support as well as duplicate key error on ordered batch
    var batch = coll.initializeOrderedBulkOp();
    batch.insert({a:1});
    batch.find({a:3}).upsert().updateOne({a:3, b:1})
    batch.insert({a:1})
    batch.insert({a:2});

    // Should throw error
    try {
        result = batch.execute({w:5, wtimeout:1});
    } catch(err) {
        error = true;
    }

    assert(error);
}

/*******************************************************************
 *
 * Unordered
 *
 *******************************************************************/
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
// executeUnorderedTests();

// Reset the function
coll.getMongo().useWriteCommands = _useWriteCommands;