(function() {
    "use strict";

    const options = {setParameter: "jsHeapLimitMB=1000"};
    const conn = MongoRunner.runMongod(options);

    // verify JSHeapLimitMB set from the shell
    var assertLimit = function() {
        assert.eq(999, getJSHeapLimitMB());
    };
    var exitCode = runMongoProgram("mongo",
                                   conn.host,
                                   "--jsHeapLimitMB",
                                   999,
                                   "--eval",
                                   "(" + assertLimit.toString() + ")();");
    assert.eq(0, exitCode);

    // verify the JSHeapLimitMB set from Mongod
    const db = conn.getDB('test');
    const res = db.adminCommand({getParameter: 1, jsHeapLimitMB: 1});
    assert.commandWorked(res);
    assert.eq(1000, res.jsHeapLimitMB);

    MongoRunner.stopMongod(conn);
})();