// checks that db.serverStatus will not throw errors when metrics tree is not present

(function() {
    "use strict";
    // Test the getActiveCommands function
    // Should remove the listCollections section but keep the rest
    var testInput = {
        "isMaster": {"failed": NumberLong(0), "total": NumberLong(3)},
        "mapreduce": {"shardedfinish": {"failed": NumberLong(0), "total": NumberLong(1)}},
        "listCollections": {"failed": NumberLong(0), "total": NumberLong(0)}
    };
    var testExpected = {
        "isMaster": {"failed": NumberLong(0), "total": NumberLong(3)},
        "mapreduce": {"shardedfinish": {"failed": NumberLong(0), "total": NumberLong(1)}}
    };
    var testResult = getActiveCommands(testInput);

    assert.eq(testResult, testExpected, "getActiveCommands did not return the expected result");

    // Test that the serverstatus helper works
    var result = db.serverStatus();
    assert.neq(undefined, result, result);
    // Test that the metrics tree returns
    assert.neq(undefined, result.metrics, result);
    // Test that the metrics.commands tree returns
    assert.neq(undefined, result.metrics.commands, result);
    // Test that the metrics.commands.serverStatus value is non-zero
    assert.neq(0, result.metrics.commands.serverStatus.total, result);

    // Test that the command returns successfully when no metrics tree is present
    var result = db.serverStatus({"metrics": 0});
    assert.eq(undefined, result.metrics, result);
}());