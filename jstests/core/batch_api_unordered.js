var collectionName = "batch_api_unordered";
var coll = db.getCollection(collectionName);
coll.drop();

jsTest.log("Starting unordered batch tests...");

var request;
var result;

jsTest.log("Starting batch api unordered tests...");

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
	batch.find({a:2}).upsert().updateOne({$set: {b:2}});
	batch.insert({a:3});
	batch.find({a:3}).remove({a:3});
	var result = batch.execute();
	assert.eq(5, result.n);
	assert(1, result.getErrorCount());
	var upserts = result.getUpsertedIds();
	assert.eq(1, upserts.length);
	assert.eq(2, upserts[0].index);
	assert(upserts[0]._id != null);
	var upsert = result.getUpsertedIdAt(0);
	assert.eq(2, upsert.index);
	assert(upsert._id != null);

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
	assert.eq(2, result.n);
	assert.eq(true, result.hasErrors());
	assert(1, result.getErrorCount());

	// Get the top level error
	var error = result.getSingleError();
	assert.eq(65, error.code);
	assert(error.errmsg != null);

	// Get the first error
	var error = result.getErrorAt(0);
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
	assert.eq(3, result.n);
	assert.eq(true, result.hasErrors());
	assert(3, result.getErrorCount());

	// Individual error checking
	var error = result.getErrorAt(0);
	assert.eq(1, error.index);
	assert.eq(11000, error.code);
	assert(error.errmsg != null);
	assert.eq(2, error.getOperation().q.b);
	assert.eq(1, error.getOperation().u['$set'].a);
	assert.eq(false, error.getOperation().multi);
	assert.eq(true, error.getOperation().upsert);

	var error = result.getErrorAt(1);
	assert.eq(3, error.index);
	assert.eq(11000, error.code);
	assert(error.errmsg != null);
	assert.eq(2, error.getOperation().q.b);
	assert.eq(1, error.getOperation().u['$set'].a);
	assert.eq(false, error.getOperation().multi);
	assert.eq(true, error.getOperation().upsert);

	var error = result.getErrorAt(2);
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

// Save the existing useWriteCommands function
var _useWriteCommands = coll.getMongo().useWriteCommands;

// Force the use of useWriteCommands
coll._mongo.useWriteCommands = function() {
	return true;
}

// Execute tests using legacy operations
executeTests();

// Force the use of legacy commands
coll._mongo.useWriteCommands = function() {
	return false;
}

// Execute tests using legacy operations
executeTests();

// Reset the function
coll.getMongo().useWriteCommands = _useWriteCommands;