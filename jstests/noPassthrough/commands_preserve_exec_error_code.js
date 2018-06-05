// Tests that an error encountered during PlanExecutor execution will be propagated back to the user
// with the original error code. This is important for retryable errors like
// 'InterruptedDueToStepDown',
// and also to ensure that the error is not swallowed and the diagnostic info is not lost.
(function() {
    "use strict";

    const mongod = MongoRunner.runMongod({});
    assert.neq(mongod, null, "mongod failed to start up");
    const db = mongod.getDB("test");
    const coll = db.commands_preserve_exec_error_code;
    coll.drop();

    assert.writeOK(coll.insert([{_id: 0}, {_id: 1}, {_id: 2}]));
    assert.commandWorked(coll.createIndex({geo: "2d"}));

    assert.commandWorked(
        db.adminCommand({configureFailPoint: "planExecutorAlwaysFails", mode: "alwaysOn"}));

    function assertFailsWithInternalError(fn) {
        const error = assert.throws(fn);
        assert.eq(error.code, ErrorCodes.InternalError, tojson(error));
        assert.neq(-1,
                   error.message.indexOf("planExecutorAlwaysFails"),
                   "Expected error message to be preserved");
    }
    function assertCmdFailsWithInternalError(cmd) {
        const res =
            assert.commandFailedWithCode(db.runCommand(cmd), ErrorCodes.InternalError, tojson(cmd));
        assert.neq(-1,
                   res.errmsg.indexOf("planExecutorAlwaysFails"),
                   "Expected error message to be preserved");
    }

    assertFailsWithInternalError(() => coll.find().itcount());
    assertFailsWithInternalError(() => coll.updateOne({_id: 1}, {$set: {x: 2}}));
    assertFailsWithInternalError(() => coll.deleteOne({_id: 1}));
    assertFailsWithInternalError(() => coll.count({_id: 1}));
    assertFailsWithInternalError(() => coll.aggregate([]).itcount());
    assertCmdFailsWithInternalError({distinct: coll.getName(), key: "_id"});
    assertCmdFailsWithInternalError({geoNear: coll.getName(), near: [0, 0]});
    assertCmdFailsWithInternalError(
        {findAndModify: coll.getName(), query: {_id: 1}, update: {$set: {x: 2}}});

    assert.commandWorked(
        db.adminCommand({configureFailPoint: "planExecutorAlwaysFails", mode: "off"}));
    MongoRunner.stopMongod(mongod);
}());
