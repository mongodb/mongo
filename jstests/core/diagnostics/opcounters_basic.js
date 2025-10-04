// Checks that db.serverStatus will not throw errors when metrics tree is not present.

{
    const result = db.serverStatus().metrics.commands;
    // Test that the metrics.commands.serverStatus value is non-zero
    assert.neq(0, db.serverStatus().metrics.commands.serverStatus.total, tojson(result));
}
{
    // Test that the command returns successfully when no metrics tree is present
    const result = db.serverStatus({"metrics": 0});
    assert.eq(undefined, result.metrics, tojson(result));
}
