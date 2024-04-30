/**
 * Test the convertToCapped cmd.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: convertToCapped.
 *   not_allowed_with_signed_security_token,
 *   requires_non_retryable_commands,
 *   requires_capped,
 *   # Sharded collections can't be capped.
 *   assumes_unsharded_collection,
 * ]
 */

let testDb = db.getSiblingDB("convert_to_capped");
let coll = testDb.coll;
testDb.dropDatabase();

// Create a collection with some data.
let num = 10;
for (let i = 0; i < num; ++i) {
    assert.commandWorked(coll.insert({_id: i}));
}
assert(!coll.isCapped());

// Command must fail if size is not specified.
{
    assert.commandFailedWithCode(testDb.runCommand({convertToCapped: coll.getName()}),
                                 ErrorCodes.InvalidOptions);
    assert(!coll.isCapped());
}

// Ensure we do not allow overflowing the size long long on the server (SERVER-33078).
{
    assert.commandFailedWithCode(
        testDb.runCommand({convertToCapped: coll.getName(), size: 5308156746568725891247}),
        ErrorCodes.BadValue);
    assert(!coll.isCapped());
}

// Can't cap a timeseries collection.
{
    const timeseriesCollName = "timeseriesColl";

    assert.commandWorked(testDb.runCommand(
        {create: timeseriesCollName, timeseries: {timeField: "time", metaField: "meta"}}));

    const timeseriesColl = testDb.getCollection(timeseriesCollName);
    assert(!timeseriesColl.isCapped());

    assert.commandFailedWithCode(
        testDb.runCommand({convertToCapped: timeseriesColl.getName(), size: 1000}),
        [ErrorCodes.CommandNotSupportedOnView, ErrorCodes.IllegalOperation]);
    assert(!timeseriesColl.isCapped());
}

// Can't cap a view.
{
    const viewName = "viewNss";
    assert.commandWorked(testDb.createView(viewName, coll.getName(), [{$match: {x: 1}}]));

    assert.commandFailedWithCode(testDb.runCommand({convertToCapped: viewName, size: 1000}),
                                 ErrorCodes.CommandNotSupportedOnView);

    assert(!testDb.getCollection(viewName).isCapped());
    assert(!coll.isCapped());
}

// Can't cap a collection that doesn't exist.
{
    assert.commandFailedWithCode(
        testDb.runCommand({convertToCapped: "nonExistingColl", size: 1000}),
        ErrorCodes.NamespaceNotFound);
}

// Proper command should work.
{
    assert.commandWorked(testDb.runCommand({convertToCapped: coll.getName(), size: 1000}));
    assert(coll.isCapped());

    // Calling convertToCapped over an already capped collection should work
    assert.commandWorked(testDb.runCommand({convertToCapped: coll.getName(), size: 1000}));
    assert(coll.isCapped());
}
