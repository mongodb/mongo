// Server-side Javascript is deprecated in 8.0
//
// In this test, we run queries with $where, $function, and $accumulator multiple times.
// We want to make sure that the deprecation warning message is only logged twice despite
// the multiple invocations in an effort to not clutter the dev's console.
// More specifically, we expect to only log 1 out of 128 events.
// @tags: [requires_scripting]

import {iterateMatchingLogLines} from "jstests/libs/log.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = 'testDB';
const collName = 'testColl';

const whereCmdObj = {
    find: collName,
    filter: {
        $where: function() {
            return this.cust_id.split("").reverse().join("") == "FED";
        }
    }
};

const accumulatorCmdObj = {
    aggregate: collName,
    cursor: {},
    pipeline: [{
        $group: {
            _id: "$cust_id",
            idCount: {
                $accumulator: {
                    init: function() {
                        return 0;
                    },
                    accumulateArgs: ["$val"],
                    accumulate: function(state, val) {
                        return state + val;
                    },
                    merge: function(state1, state2) {
                        return state1 + state2;
                    },
                    finalize: function(state) {
                        return state;
                    }
                }
            }
        }
    }],
};

const functionFindCmdObj = {
    find: collName,
    filter: {
        $expr: {
            $function: {
                body: function(id) {
                    return id.split("").reverse().join("") == "FED";
                },
                args: ["$cust_id"],
                lang: "js"
            }
        }
    }
};

const functionAggCmdObj = {
    aggregate: collName,
    cursor: {},
    pipeline: [{
        $project: {
            newValue: {
                $function: {
                    args: ["$amount", -1],
                    body: function(first, second) {
                        return first + second;
                    },
                    lang: "js",
                },
            },
            _id: 0,
        }
    }],
};

const whereDeprecationMsg = {
    msg:
        "$where is deprecated. For more information, see https://www.mongodb.com/docs/manual/reference/operator/query/where/"
};

const functionDeprecationMsg = {
    msg:
        "$function is deprecated. For more information, see https://www.mongodb.com/docs/manual/reference/operator/aggregation/function/"
};

const accumulatorDeprecationMsg = {
    msg:
        "$accumulator is deprecated. For more information, see https://www.mongodb.com/docs/manual/reference/operator/aggregation/accumulator/"
};

const data = [
    {cust_id: "ABC", amount: 100},
    {cust_id: "ABC", amount: 200},
    {cust_id: "DEF", amount: 50},
    {cust_id: "ABC", amount: 10},
    {cust_id: "ABC", amount: 20},
    {cust_id: "DEF", amount: 5},
];

function checkLogs(db, deprecationMsg, numLogLines) {
    const globalLogs = db.adminCommand({getLog: 'global'});
    const matchingLogLines = [...iterateMatchingLogLines(globalLogs.log, deprecationMsg)];
    assert.eq(matchingLogLines.length, numLogLines, matchingLogLines);
}

/**
 * This function will test for the logging of the deprecation messages for different operators
 * that use server-side Javascript. The server logs for the first tick of an expression and then
 * every 128 ticks, so the query is run a total of 129 times. The test can run either on mongod
 * or mongos, and supports both find and agg. If a ShardingTest is provided, the primary shard's
 * logs will also be checked.
 *
 * db:             The specified mongod or mongos in which the query is executed.
 * deprecationMsg: The expected log message for an expression.
 * command:        The agg or find command involving only one of the expressions.
 * shards:         The ShardingTest containing the shards that pair up with the mongos. Null by
 *                 default to accomodate for the mongod case.
 */
function deprecationTest(db, deprecationMsg, command, shards = null) {
    assert.commandWorked(db.adminCommand({clearLog: "global"}));

    // Assert that deprecation msg is not logged before the command is even run.
    checkLogs(db, deprecationMsg, 0);

    if (shards) {
        assert.commandWorked(shards.shard0.getDB(dbName).adminCommand({clearLog: "global"}));

        // Same as above but in the primary shard.
        checkLogs(shards.shard0.getDB(dbName), deprecationMsg, 0);
    }

    assert.commandWorked(db.runCommand(command));

    // Now that we have ran the command, make sure the deprecation message is logged once.
    checkLogs(db, deprecationMsg, 1);

    if (shards) {
        // Same as above but in the primary shard.
        checkLogs(shards.shard0.getDB(dbName), deprecationMsg, 1);
    }

    // Now run the query 128 more times to get another log.
    for (let i = 0; i < 128; i++) {
        assert.commandWorked(db.runCommand(command));
    }

    // We check that we only had one more log.
    checkLogs(db, deprecationMsg, 2);

    if (shards) {
        // Same as above but in the primary shard.
        checkLogs(shards.shard0.getDB(dbName), deprecationMsg, 2);

        // Check that we didn't log on other shards but the primary.
        checkLogs(shards.shard1.getDB(dbName), deprecationMsg, 0);
    }
}

jsTest.log('Test standalone');

const standalone = MongoRunner.runMongod({});
const standaloneDB = standalone.getDB(dbName);
const standaloneColl = standaloneDB.getCollection(collName);

assert.commandWorked(standaloneColl.insert(data));

deprecationTest(standaloneDB, whereDeprecationMsg, whereCmdObj);
deprecationTest(standaloneDB, accumulatorDeprecationMsg, accumulatorCmdObj);
deprecationTest(standaloneDB, functionDeprecationMsg, functionAggCmdObj);
deprecationTest(standaloneDB, functionDeprecationMsg, functionFindCmdObj);

MongoRunner.stopMongod(standalone);

jsTest.log('Test sharded');

const shards = new ShardingTest({shards: 2, mongos: 1});
const session = shards.s.getDB(dbName).getMongo().startSession();
const shardedDB = session.getDatabase(dbName);

assert.commandWorked(shards.s0.adminCommand(
    {enableSharding: shardedDB.getName(), primaryShard: shards.shard0.shardName}));

const shardedColl = shardedDB.testColl;

assert.commandWorked(shardedDB.createCollection(shardedColl.getName()));

assert.commandWorked(
    shards.s0.adminCommand({shardCollection: shardedColl.getFullName(), key: {_id: 1}}));

assert.commandWorked(shardedColl.insert(data));

deprecationTest(shardedDB, whereDeprecationMsg, whereCmdObj, shards);
deprecationTest(shardedDB, accumulatorDeprecationMsg, accumulatorCmdObj, shards);
deprecationTest(shardedDB, functionDeprecationMsg, functionAggCmdObj, shards);
deprecationTest(shardedDB, functionDeprecationMsg, functionFindCmdObj, shards);

shards.stop();
