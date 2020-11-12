var collectionName = "bulk_api_limits";
var coll = db.getCollection(collectionName);
coll.drop();

jsTest.log("Starting unordered bulk tests...");

var request;
var result;

/********************************************************
 *
 * Ordered tests should return same results for write command as
 * well as for the legacy operations
 *
 *******************************************************/
var executeTestsUnordered = function() {
    // Create unique index
    coll.dropIndexes();
    coll.remove({});
    coll.ensureIndex({a: 1}, {unique: true});

    /**
     * Fail during batch construction due to single document > maxBSONSize
     */
    // Set up a giant string to blow through the max message size
    var hugeString = "";
    // Create it bigger than 16MB
    for (var i = 0; i < (1024 * 1100); i++) {
        hugeString = hugeString + "1234567890123456";
    }

    // Set up the batch
    var batch = coll.initializeUnorderedBulkOp();
    batch.insert({b: 1, a: 1});
    // Should fail on insert due to string being to big
    try {
        batch.insert({string: hugeString});
        assert(false);
    } catch (err) {
    }

    // Create unique index
    coll.dropIndexes();
    coll.remove({});

    /**
     * Check that batch is split when documents overflow the BSON size
     */
    // Set up a giant string to blow through the max message size
    var hugeString = "";
    // Create 4 MB strings to test splitting
    for (var i = 0; i < (1024 * 256); i++) {
        hugeString = hugeString + "1234567890123456";
    }

    // Insert the string a couple of times, should force split into multiple batches
    var batch = coll.initializeUnorderedBulkOp();
    batch.insert({a: 1, b: hugeString});
    batch.insert({a: 2, b: hugeString});
    batch.insert({a: 3, b: hugeString});
    batch.insert({a: 4, b: hugeString});
    batch.insert({a: 5, b: hugeString});
    batch.insert({a: 6, b: hugeString});
    var result = batch.execute();
    printjson(JSON.stringify(result));

    // Basic properties check
    assert.eq(6, result.nInserted);
    assert.eq(false, result.hasWriteErrors());
};

/********************************************************
 *
 * Ordered tests should return same results for write command as
 * well as for the legacy operations
 *
 *******************************************************/
var executeTestsOrdered = function() {
    /**
     * Fail during batch construction due to single document > maxBSONSize
     */
    // Set up a giant string to blow through the max message size
    var hugeString = "";
    // Create it bigger than 16MB
    for (var i = 0; i < (1024 * 1100); i++) {
        hugeString = hugeString + "1234567890123456";
    }

    // Set up the batch
    var batch = coll.initializeOrderedBulkOp();
    batch.insert({b: 1, a: 1});
    // Should fail on insert due to string being to big
    try {
        batch.insert({string: hugeString});
        assert(false);
    } catch (err) {
    }

    // Create unique index
    coll.dropIndexes();
    coll.remove({});

    /**
     * Check that batch is split when documents overflow the BSON size
     */
    // Set up a giant string to blow through the max message size
    var hugeString = "";
    // Create 4 MB strings to test splitting
    for (var i = 0; i < (1024 * 256); i++) {
        hugeString = hugeString + "1234567890123456";
    }

    // Insert the string a couple of times, should force split into multiple batches
    var batch = coll.initializeOrderedBulkOp();
    batch.insert({a: 1, b: hugeString});
    batch.insert({a: 2, b: hugeString});
    batch.insert({a: 3, b: hugeString});
    batch.insert({a: 4, b: hugeString});
    batch.insert({a: 5, b: hugeString});
    batch.insert({a: 6, b: hugeString});
    var result = batch.execute();

    // Basic properties check
    assert.eq(6, result.nInserted);
    assert.eq(false, result.hasWriteErrors());

    // Create unique index
    coll.dropIndexes();
    coll.remove({});
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
    executeTestsUnordered();
    executeTestsOrdered();
}

// Force the use of legacy commands
coll._mongo.useWriteCommands = function() {
    return false;
};

// Execute tests using legacy operations
executeTestsUnordered();
executeTestsOrdered();

// Reset the function
coll.getMongo().useWriteCommands = _useWriteCommands;