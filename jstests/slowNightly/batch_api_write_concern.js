/**
 * Fail Due to no journal and requesting journal write concern
 */
var m = MongoRunner.runMongod({ "nojournal" : "" });
var db = m.getDB("test");

var coll = db.getCollection("batch_api_write_concern");
coll.drop();

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
	var result = batch.execute({j:true});
	assert.eq(2, result.n);
	assert.eq(65, result.getSingleError().code);
	assert(typeof result.getSingleError().errmsg == 'string');
	assert.eq(true, result.hasErrors());
	assert.eq(2, result.getErrorCount());
	assert.eq(2, result.getWCErrors().length);

	// Test errors for expected behavior
	assert.eq(0, result.getErrorAt(0).index);
	assert.eq(64, result.getErrorAt(0).code);
	assert(typeof result.getErrorAt(0).errmsg == 'string');
	assert.eq(1, result.getErrorAt(0).getOperation().a);

	//
	// Fail due to write concern support
	var batch = coll.initializeOrderedBulkOp();
	batch.insert({a:1});
	batch.insert({a:2});
	var result = batch.execute({w:2, wtimeout:1000});
	assert.eq(2, result.n);
	assert.eq(65, result.getSingleError().code);
	assert(typeof result.getSingleError().errmsg == 'string');
	assert.eq(true, result.hasErrors());
	assert.eq(2, result.getErrorCount());
	assert.eq(2, result.getWCErrors().length);

	// Test errors for expected behavior
	assert.eq(0, result.getErrorAt(0).index);
	assert.eq(64, result.getErrorAt(0).code);
	assert(result.getErrorAt(0).errmsg.indexOf("no replication") != -1);
	assert.eq(1, result.getErrorAt(0).getOperation().a);

	// Create unique index
	coll.remove({});
	coll.ensureIndex({a : 1}, {unique : true});

	//
	// Fail with write concern error and duplicate key error
	var batch = coll.initializeOrderedBulkOp();
	batch.insert({a:1});
	batch.insert({a:1});
	batch.insert({a:2});
	var result = batch.execute({w:2, wtimeout:1000});
	assert.eq(1, result.n);
	assert.eq(65, result.getSingleError().code);
	assert(typeof result.getSingleError().errmsg == 'string');
	assert.eq(true, result.hasErrors());
	assert.eq(2, result.getErrorCount());
	assert.eq(1, result.getWCErrors().length);

	// Test errors for expected behavior
	assert.eq(0, result.getErrorAt(0).index);
	assert.eq(64, result.getErrorAt(0).code);
	assert(result.getErrorAt(0).errmsg.indexOf("no replication") != -1);
	assert.eq(1, result.getErrorAt(0).getOperation().a);

	// Test errors for expected behavior
	assert.eq(1, result.getErrorAt(1).index);
	assert.eq(11000, result.getErrorAt(1).code);
	assert(result.getErrorAt(1).errmsg.indexOf("duplicate") != -1);
	assert.eq(1, result.getErrorAt(1).getOperation().a);

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
	assert.eq(3, result.n);
	assert.eq(65, result.getSingleError().code);
	assert(typeof result.getSingleError().errmsg == 'string');
	assert.eq(true, result.hasErrors());
	assert.eq(3, result.getErrorCount());
	assert.eq(3, result.getWCErrors().length);

	// Test errors for expected behavior
	assert.eq(0, result.getErrorAt(0).index);
	assert.eq(64, result.getErrorAt(0).code);
	assert(typeof result.getErrorAt(0).errmsg == 'string');
	assert.eq(1, result.getErrorAt(0).getOperation().a);

	assert.eq(1, result.getErrorAt(1).index);
	assert.eq(64, result.getErrorAt(1).code);
	assert(typeof result.getErrorAt(1).errmsg == 'string');
	assert.eq(3, result.getErrorAt(1).getOperation().q.a);

	assert.eq(2, result.getErrorAt(2).index);
	assert.eq(64, result.getErrorAt(2).code);
	assert(typeof result.getErrorAt(2).errmsg == 'string');
	assert.eq(2, result.getErrorAt(2).getOperation().a);

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
	assert.eq(3, result.n);
	assert.eq(65, result.getSingleError().code);
	assert(typeof result.getSingleError().errmsg == 'string');
	assert.eq(true, result.hasErrors());
	assert.eq(3, result.getErrorCount());
	assert.eq(3, result.getWCErrors().length);

	// Test errors for expected behavior
	assert.eq(0, result.getErrorAt(0).index);
	assert.eq(64, result.getErrorAt(0).code);
	assert(typeof result.getErrorAt(0).errmsg == 'string');
	assert.eq(1, result.getErrorAt(0).getOperation().a);

	assert.eq(1, result.getErrorAt(1).index);
	assert.eq(64, result.getErrorAt(1).code);
	assert(typeof result.getErrorAt(1).errmsg == 'string');
	assert.eq(3, result.getErrorAt(1).getOperation().q.a);

	assert.eq(2, result.getErrorAt(2).index);
	assert.eq(64, result.getErrorAt(2).code);
	assert(typeof result.getErrorAt(2).errmsg == 'string');
	assert.eq(2, result.getErrorAt(2).getOperation().a);

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
	assert.eq(3, result.n);
	assert.eq(65, result.getSingleError().code);
	assert(typeof result.getSingleError().errmsg == 'string');
	assert.eq(true, result.hasErrors());
	assert.eq(4, result.getErrorCount());
	assert.eq(3, result.getWCErrors().length);
	assert.eq(1, result.getWriteErrors().length);
	assert.eq(4, result.getErrors().length);

	// Get the non wc error
	assert.eq(2, result.getWriteErrors()[0].index);
	assert.eq(11000, result.getWriteErrors()[0].code);
	assert(typeof result.getWriteErrors()[0].errmsg == 'string');
	assert.eq(1, result.getWriteErrors()[0].getOperation().a);

	// Test errors for expected behavior
	assert.eq(0, result.getErrorAt(0).index);
	assert.eq(64, result.getErrorAt(0).code);
	assert(typeof result.getErrorAt(0).errmsg == 'string');
	assert.eq(1, result.getErrorAt(0).getOperation().a);

	assert.eq(1, result.getErrorAt(1).index);
	assert.eq(64, result.getErrorAt(1).code);
	assert(typeof result.getErrorAt(1).errmsg == 'string');
	assert.eq(3, result.getErrorAt(1).getOperation().q.a);

	assert.eq(2, result.getErrorAt(2).index);
	assert.eq(11000, result.getErrorAt(2).code);
	assert(typeof result.getErrorAt(2).errmsg == 'string');
	assert.eq(1, result.getErrorAt(2).getOperation().a);

	assert.eq(3, result.getErrorAt(3).index);
	assert.eq(64, result.getErrorAt(3).code);
	assert(typeof result.getErrorAt(3).errmsg == 'string');
	assert.eq(2, result.getErrorAt(3).getOperation().a);

	var upserts = result.getUpsertedIds();
	assert.eq(1, upserts.length);
	assert.eq(1, upserts[0].index);
	assert(upserts[0]._id != null);
}

// Save the existing useWriteCommands function
var _useWriteCommands = coll.getMongo().useWriteCommands;

// Force the use of useWriteCommands
coll._mongo.useWriteCommands = function() {
	return true;
}

// Execute tests using legacy operations
executeOrderedTests();
executeUnorderedTests();

// Force the use of legacy commands
coll._mongo.useWriteCommands = function() {
	return false;
}

// Execute tests using legacy operations
executeOrderedTests();
executeUnorderedTests();

// Reset the function
coll.getMongo().useWriteCommands = _useWriteCommands;