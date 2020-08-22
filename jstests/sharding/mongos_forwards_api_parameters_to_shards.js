/**
 * When a client calls a mongos command with API parameters, mongos must forward them to shards.
 *
 * @tags: [multiversion_incompatible]
 */

(function() {
'use strict';

load('jstests/sharding/libs/sharded_transactions_helpers.js');

let st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 1, setParameter: {logComponentVerbosity: tojson({command: {verbosity: 2}})}}
});

class APIParameterTest {
    constructor(
        command,
        {dbName = "db", inAPIVersion1 = true, permittedInTxn = true, shardCommandName} = {}) {
        this.command = command;
        this.dbName = dbName;
        this.inAPIVersion1 = inAPIVersion1;
        this.permittedInTxn = permittedInTxn;
        if (shardCommandName === undefined) {
            this.commandName = Object.keys(command)[0];
        } else {
            // mongos executes a different command on the shards, e.g. mapReduce becomes aggregate.
            this.commandName = shardCommandName;
        }
    }
}

const tests = [
    // Write commands. Note, these rely on _id 1 residing on shard 0.
    new APIParameterTest({insert: "collection", documents: [{_id: 1}]}),
    new APIParameterTest({update: "collection", updates: [{q: {_id: 1}, u: {$set: {x: 1}}}]}),
    new APIParameterTest({delete: "collection", deletes: [{q: {_id: 1}, limit: 1}]}),

    // Read commands.
    new APIParameterTest({aggregate: "collection", pipeline: [], cursor: {}}),
    new APIParameterTest({aggregate: "collection", pipeline: [], cursor: {}, explain: true},
                         {shardCommandName: "explain", permittedInTxn: false}),
    new APIParameterTest({find: "collection"}),
    new APIParameterTest({count: "collection"}, {permittedInTxn: false}),
    new APIParameterTest({count: "collection", query: {_id: {$lt: 0}}},
                         {inAPIVersion1: false, permittedInTxn: false}),
    new APIParameterTest({distinct: "collection", key: "_id"},
                         {inAPIVersion1: false, permittedInTxn: false}),
    new APIParameterTest(
        {
            mapReduce: "collection",
            map: function() {
                emit(1, 1);
            },
            reduce: function(key, values) {
                return {count: values.length};
            },
            out: {inline: 1}
        },
        {inAPIVersion1: false, permittedInTxn: false, shardCommandName: "aggregate"}),

    // FindAndModify.
    new APIParameterTest({findAndModify: "collection", query: {_id: 1}, remove: true}),

    // DDL. Order matters: we must create, modify, then drop an index on collection2.
    new APIParameterTest({createIndexes: "collection2", indexes: [{key: {x: 1}, name: "x_1"}]}),
    new APIParameterTest({collMod: "collection2", index: {keyPattern: {x: 1}, hidden: true}},
                         {permittedInTxn: false}),
    new APIParameterTest({dropIndexes: "collection2", index: "x_1"}, {permittedInTxn: false}),
    // We can create indexes on a non-existent collection in a sharded transaction.
    new APIParameterTest({create: "newCollection"}),
    new APIParameterTest({renameCollection: "db.newCollection", to: "db.newerCollection"},
                         {inAPIVersion1: false, permittedInTxn: false, dbName: "admin"}),
    new APIParameterTest({drop: "collection"}, {permittedInTxn: false}),
    new APIParameterTest({dropDatabase: 1}, {permittedInTxn: false}),
];

function checkPrimaryLog(conn, commandName, apiVersion, apiStrict, apiDeprecationErrors, message) {
    const logs = checkLog.getGlobalLog(conn);
    let lastCommandInvocation;

    for (let logMsg of logs) {
        const obj = JSON.parse(logMsg);
        // Search for "About to run the command" logs.
        if (obj.id !== 21965) {
            continue;
        }

        const args = obj.attr.commandArgs;
        if (commandName !== Object.keys(args)[0]) {
            continue;
        }

        lastCommandInvocation = args;
        if (args.apiVersion !== apiVersion || args.apiStrict !== apiStrict ||
            args.apiDeprecationErrors !== apiDeprecationErrors) {
            continue;
        }

        // Found a match.
        return;
    }

    if (lastCommandInvocation === undefined) {
        doassert(`Primary didn't log ${commandName}`);
        return;
    }

    doassert(`Primary didn't log ${message}, last invocation of ${commandName} was` +
             ` ${tojson(lastCommandInvocation)}`);
}

for (const sharded of [false, true]) {
    for (const [apiVersion, apiStrict, apiDeprecationErrors] of [[undefined, undefined, undefined],
                                                                 ["1", undefined, undefined],
                                                                 ["1", undefined, false],
                                                                 ["1", undefined, true],
                                                                 ["1", false, undefined],
                                                                 ["1", false, false],
                                                                 ["1", false, true],
                                                                 ["1", true, undefined],
                                                                 ["1", true, false],
                                                                 ["1", true, true],
    ]) {
        for (let inTransaction of [false, true]) {
            if (sharded) {
                jsTestLog("Sharded setup");
                assert.commandWorked(st.s.getDB("db")["collection"].insert(
                    {_id: 0}, {writeConcern: {w: "majority"}}));
                assert.commandWorked(st.s.getDB("db")["collection"].insert(
                    {_id: 20}, {writeConcern: {w: "majority"}}));

                assert.commandWorked(st.s.adminCommand({enableSharding: "db"}));
                st.ensurePrimaryShard("db", st.shard0.shardName);
                assert.commandWorked(
                    st.s.adminCommand({shardCollection: "db.collection", key: {_id: 1}}));

                // The chunk with _id 1 is on shard 0.
                assert.commandWorked(
                    st.s.adminCommand({split: "db.collection", middle: {_id: 10}}));
                assert.commandWorked(st.s.adminCommand(
                    {moveChunk: "db.collection", find: {_id: 20}, to: st.shard1.shardName}));
            } else {
                jsTestLog("Unsharded setup");
                assert.commandWorked(st.s.getDB("db")["collection"].insert(
                    {_id: 0}, {writeConcern: {w: "majority"}}));
                st.ensurePrimaryShard("db", st.shard0.shardName);
            }

            // Shard 0's primary.
            const primary = st.rs0.getPrimary();

            for (const test of tests) {
                if (inTransaction && !test.permittedInTxn) {
                    continue;
                }

                if (apiStrict && !test.inAPIVersion1) {
                    continue;
                }

                // Make a copy of the test's command body, and set its API parameters.
                const commandWithAPIParams = Object.assign({}, test.command);
                if (apiVersion !== undefined) {
                    commandWithAPIParams.apiVersion = apiVersion;
                }

                if (apiStrict !== undefined) {
                    commandWithAPIParams.apiStrict = apiStrict;
                }

                if (apiDeprecationErrors !== undefined) {
                    commandWithAPIParams.apiDeprecationErrors = apiDeprecationErrors;
                }

                assert.commandWorked(primary.adminCommand({clearLog: "global"}));
                const message = `command ${tojson(commandWithAPIParams)}` +
                    ` ${sharded ? "sharded" : "unsharded"},` +
                    ` ${inTransaction ? "in" : "outside"} transaction`;

                flushRoutersAndRefreshShardMetadata(st, {ns: "db.collection"});

                jsTestLog(`Running ${message}`);

                if (inTransaction) {
                    const session = st.s0.startSession();
                    const sessionDb = session.getDatabase(test.dbName);
                    session.startTransaction();
                    assert.commandWorked(sessionDb.runCommand(commandWithAPIParams));
                    assert.commandWorked(session.commitTransaction_forTesting());
                } else {
                    const db = st.s0.getDB(test.dbName);
                    assert.commandWorked(db.runCommand(commandWithAPIParams));
                }

                checkPrimaryLog(primary,
                                test.commandName,
                                apiVersion,
                                apiStrict,
                                apiDeprecationErrors,
                                message);
            }

            jsTestLog("JS test cleanup: Drop database 'db'");
            st.s0.getDB("db").runCommand({dropDatabase: 1});
        }
    }
}

st.stop();
})();
