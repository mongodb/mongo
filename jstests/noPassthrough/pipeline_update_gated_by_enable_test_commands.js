// Pipeline-style update fails when enableTestCommands is false.
// TODO SERVER-40397: Add test for findAndModify with pipeline.
// TODO SERVER-40419: Delete this test.

(function() {
    "use strict";

    (function testFailureWhenTestCommandsDisabled() {
        jsTest.setOption('enableTestCommands', false);
        const conn = MongoRunner.runMongod();
        const db = conn.getDB("test");

        assert.commandFailedWithCode(db.coll.update({}, [{$set: {x: 1}}]),
                                     ErrorCodes.FailedToParse);
        const error =
            assert.throws(() => db.coll.findAndModify({query: {}, update: [{$set: {x: 1}}]}));
        assert.eq(error.code, ErrorCodes.FailedToParse);

        MongoRunner.stopMongod(conn);
    }());

    (function testSuccessWhenTestCommandsEnabled() {
        jsTest.setOption('enableTestCommands', true);
        const conn = MongoRunner.runMongod();
        const db = conn.getDB("test");

        assert.commandWorked(db.coll.update({}, [{$set: {x: 1}}]));
        assert.doesNotThrow(() => db.coll.findAndModify({query: {}, update: [{$set: {x: 1}}]}));

        MongoRunner.stopMongod(conn);
    }());

}());
