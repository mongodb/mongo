// Tests that commands properly handle their underlying plan executor failing or being killed.
(function() {
    'use strict';
    const dbpath = MongoRunner.dataPath + jsTest.name();
    resetDbpath(dbpath);
    const mongod = MongoRunner.runMongod({dbpath: dbpath});
    const db = mongod.getDB("test");
    const collName = jsTest.name();
    const coll = db.getCollection(collName);

    // How many works it takes to yield.
    const yieldIterations = 2;
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: yieldIterations}));
    const nDocs = yieldIterations + 2;

    /**
     * Asserts that 'commandResult' indicates a command failure, and returns the error message.
     */
    function assertContainsErrorMessage(commandResult) {
        assert(commandResult.ok === 0 ||
                   (commandResult.ok === 1 && commandResult.writeErrors !== undefined),
               'expected command to fail: ' + tojson(commandResult));
        if (commandResult.ok === 0) {
            return commandResult.errmsg;
        } else {
            return commandResult.writeErrors[0].errmsg;
        }
    }

    function setupCollection() {
        coll.drop();
        let bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < nDocs; i++) {
            bulk.insert({_id: i, a: i});
        }
        assert.writeOK(bulk.execute());
        assert.commandWorked(coll.createIndex({a: 1}));
    }

    /**
     * Asserts that the command given by 'cmdObj' will propagate a message from a PlanExecutor
     * failure back to the user.
     */
    function assertCommandPropogatesPlanExecutorFailure(cmdObj) {
        // Make sure the command propagates failure messages.
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "planExecutorAlwaysFails", mode: "alwaysOn"}));
        let res = db.runCommand(cmdObj);
        let errorMessage = assertContainsErrorMessage(res);
        assert.neq(errorMessage.indexOf("planExecutorAlwaysFails"),
                   -1,
                   "Expected error message to include 'planExecutorAlwaysFails', instead found: " +
                       errorMessage);
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "planExecutorAlwaysFails", mode: "off"}));
    }

    /**
     * Asserts that the command properly handles failure scenarios while using its PlanExecutor.
     * Asserts that the appropriate error message is propagated if the is a failure during
     * execution, or if the plan was killed during execution. If 'options.commandYields' is false,
     * asserts that the PlanExecutor cannot be killed, and succeeds when run concurrently with any
     * of 'invalidatingCommands'.
     *
     * @param {Object} cmdObj - The command to run.
     * @param {Boolean} [options.commandYields=true] - Whether or not this command can yield during
     *   execution.
     * @param {Object} [options.curOpFilter] - The query to use to find this operation in the
     *   currentOp output. The default checks that all fields of cmdObj are in the curOp command.
     * @param {Function} [options.customSetup=undefined] - A callback to do any necessary setup
     *   before the command can be run, like adding a geospatial index before a geoNear command.
     */
    function assertCommandPropogatesPlanExecutorKillReason(cmdObj, options) {
        options = options || {};

        var curOpFilter = options.curOpFilter;
        if (!curOpFilter) {
            curOpFilter = {};
            for (var arg in cmdObj) {
                curOpFilter['command.' + arg] = {$eq: cmdObj[arg]};
            }
        }

        // These are commands that will cause all running PlanExecutors to be invalidated, and the
        // error messages that should be propagated when that happens.
        const invalidatingCommands = [
            {command: {dropDatabase: 1}, message: 'database dropped'},
            {command: {drop: collName}, message: 'collection dropped'},
            {command: {dropIndexes: collName, index: {a: 1}}, message: 'index \'a_1\' dropped'},
        ];

        for (let invalidatingCommand of invalidatingCommands) {
            setupCollection();
            if (options.customSetup !== undefined) {
                options.customSetup();
            }

            // Enable a failpoint that causes PlanExecutors to hang during execution.
            assert.commandWorked(
                db.adminCommand({configureFailPoint: "setYieldAllLocksHang", mode: "alwaysOn"}));

            const canYield = options.commandYields === undefined || options.commandYields;
            // Start a parallel shell to run the command. This should hang until we unset the
            // failpoint.
            let awaitCmdFailure = startParallelShell(`
let assertContainsErrorMessage = ${ assertContainsErrorMessage.toString() };
let res = db.runCommand(${ tojson(cmdObj) });
if (${ canYield }) {
    let errorMessage = assertContainsErrorMessage(res);
    assert.neq(errorMessage.indexOf(${ tojson(invalidatingCommand.message) }),
               -1,
                "Expected error message to include '" +
                    ${ tojson(invalidatingCommand.message) } +
                    "', instead found: " + errorMessage);
} else {
    assert.commandWorked(
        res,
        'expected non-yielding command to succeed: ' + tojson(${ tojson(cmdObj) })
    );
}
`,
                                                     mongod.port);

            // Wait until we can see the command running.
            assert.soon(
                function() {
                    if (!canYield) {
                        // The command won't yield, so we won't necessarily see it in currentOp.
                        return true;
                    }
                    return db.currentOp({
                                 $and: [
                                     {
                                       ns: coll.getFullName(),
                                       numYields: {$gt: 0},
                                     },
                                     curOpFilter,
                                 ]
                             }).inprog.length > 0;
                },
                function() {
                    return 'expected to see command yielded in currentOp output. Command: ' +
                        tojson(cmdObj) + '\n, currentOp output: ' + tojson(db.currentOp().inprog);
                });

            // Run the command that invalidates the PlanExecutor, then allow the PlanExecutor to
            // proceed.
            jsTestLog("Running invalidating command: " + tojson(invalidatingCommand.command));
            assert.commandWorked(db.runCommand(invalidatingCommand.command));
            assert.commandWorked(
                db.adminCommand({configureFailPoint: "setYieldAllLocksHang", mode: "off"}));
            awaitCmdFailure();
        }

        setupCollection();
        if (options.customSetup !== undefined) {
            options.customSetup();
        }
        assertCommandPropogatesPlanExecutorFailure(cmdObj);
    }

    // Disable aggregation's batching behavior, since that can prevent the PlanExecutor from being
    // active during the command that would have caused it to be killed.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalDocumentSourceCursorBatchSizeBytes: 1}));
    assertCommandPropogatesPlanExecutorKillReason({aggregate: collName, pipeline: [], cursor: {}});

    assertCommandPropogatesPlanExecutorKillReason({dataSize: coll.getFullName()},
                                                  {commandYields: false});

    assertCommandPropogatesPlanExecutorKillReason("dbHash", {commandYields: false});

    assertCommandPropogatesPlanExecutorKillReason({count: collName, query: {_id: {$gte: 0}}});

    assertCommandPropogatesPlanExecutorKillReason({distinct: collName, key: "_id", query: {}});

    assertCommandPropogatesPlanExecutorKillReason(
        {findAndModify: collName, filter: {fakeField: {$gt: 0}}, update: {$inc: {a: 1}}});

    assertCommandPropogatesPlanExecutorKillReason(
        {geoNear: collName, near: {type: "Point", coordinates: [0, 0]}, spherical: true}, {
            customSetup: function() {
                assert.commandWorked(coll.createIndex({geoField: "2dsphere"}));
            }
        });

    assertCommandPropogatesPlanExecutorKillReason({find: coll.getName(), filter: {}});

    assertCommandPropogatesPlanExecutorKillReason(
        {update: coll.getName(), updates: [{q: {}, u: {$set: {a: 1}}}]},
        {curOpFilter: {op: 'update'}});

    assertCommandPropogatesPlanExecutorKillReason(
        {delete: coll.getName(), deletes: [{q: {}, limit: 0}]}, {curOpFilter: {op: 'remove'}});
    MongoRunner.stopMongod(mongod);
})();
