/**
 * A large `syncdelay` set via the command line or `setParameter` can cause a precision loss
 * exception when being converted to a duration. This test exercises the protection of the command
 * line `--syncdelay` parameter and calling `setParameter`.
 */
(function() {
    var conn = MongoRunner.runMongod({storageEngine: 'mmapv1', syncdelay: 18446744073709552000});
    assert.eq(conn, null);

    conn = MongoRunner.runMongod({storageEngine: 'mmapv1'});
    assert.neq(conn, null);
    var res = conn.adminCommand({setParameter: 1, 'syncdelay': 18446744073709552000});
    assert.commandFailedWithCode(res, 2);
    assert.gt(res["errmsg"].indexOf("syncdelay must be between"), -1);
})();
