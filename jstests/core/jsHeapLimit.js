(function() {
    "use strict";

    const options = {setParameter: "jsHeapMBLimit=1000"};
    const conn = MongoRunner.runMongod(options);

    // verify JSHeapMBLimit set from the shell
    var assertLimit = function() {
        assert.eq(999, getJSHeapMBLimit());
    };
    var exitCode = runMongoProgram(
        "mongo", conn.host, "--jsHeapMBLimit", 999, "--eval", "(" + assertLimit.toString() + ")();");
    assert.eq(0, exitCode);

    // verify the JSHeapMBLimit set from Mongod
    const db = conn.getDB('test');
    const res = db.adminCommand({getParameter: 1, jsHeapMBLimit: 1});
    assert.commandWorked(res);
    assert.eq(1000, res.jsHeapMBLimit);

    MongoRunner.stopMongod(conn);
})();