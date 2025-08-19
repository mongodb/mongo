/**
 * The Javascript Just In Time compiler is deprecated in 7.0.
 *
 * This test checks that we log the deprecation warning correctly when setting the server parameter
 * on startup or on runtime.
 */
(function() {
'use strict';
load("jstests/libs/log.js");  // For findMatchingLogLine.

const dbName = 'testDB';

function checkLogs(db, deprecationMsg, numMatchingLines) {
    const globalLogs = db.adminCommand({getLog: 'global'});
    const matchingLogLines = [...findMatchingLogLines(globalLogs.log, deprecationMsg)];
    assert.eq(matchingLogLines.length, numMatchingLines, matchingLogLines);
}

/**
 * This function will test for the logging of the deprecation messages for the disableJavaScriptJIT
 * server parameter. The test can run either on mongod or mongos.
 *
 * db:             The specified mongod or mongos.
 * deprecationMsg: The expected log message.
 * command:        The setParameter command.
 * shards:         The ShardingTest containing the shards that pair up with the mongos. Null by
 *                 default to accomodate for the mongod case.
 */
function deprecationTest(db, deprecationMsg, command, shards = null) {
    // We set the parameter on startup so the message should get logged.
    checkLogs(db, deprecationMsg, 1);

    if (shards) {
        // Same as above but in the shards.
        checkLogs(shards.shard0.getDB(db), deprecationMsg, 1);
        checkLogs(shards.shard1.getDB(db), deprecationMsg, 1);
    }

    // We change the parameter during runtime to log the message again.
    assert.commandWorked(db.adminCommand(command));
    checkLogs(db, deprecationMsg, 2);
    if (shards) {
        // Same as above but in the shards.
        assert.commandWorked(shards.shard0.getDB(db).adminCommand(command));
        assert.commandWorked(shards.shard1.getDB(db).adminCommand(command));
        checkLogs(shards.shard0.getDB(db), deprecationMsg, 2);
        checkLogs(shards.shard1.getDB(db), deprecationMsg, 2);
    }
}

const deprecationMessage = {
    msg: "The Javascript Just In Time compiler is deprecated, and should be disabled " +
        "for security reasons. The server parameter will be removed in a future major " +
        "version. See https://www.mongodb.com/docs/manual/reference/parameters for " +
        "more information."
};

const setParameterCmdObj = {
    setParameter: 1,
    disableJavaScriptJIT: false
};

jsTest.log('Test standalone');

const standaloneWithKnob = MongoRunner.runMongod({setParameter: {disableJavaScriptJIT: false}});
const standaloneWithKnobDB = standaloneWithKnob.getDB(dbName);

deprecationTest(standaloneWithKnobDB, deprecationMessage, setParameterCmdObj);

MongoRunner.stopMongod(standaloneWithKnob);

jsTest.log('Test sharded');

const shardsWithKnob = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        shardOptions: {setParameter: {disableJavaScriptJIT: false}},
        mongosOptions: {setParameter: {disableJavaScriptJIT: false}}
    }
});
const sessionWithKnob = shardsWithKnob.s.getDB(dbName).getMongo().startSession();
const shardedWithKnobDB = sessionWithKnob.getDatabase(dbName);

deprecationTest(shardedWithKnobDB, deprecationMessage, setParameterCmdObj, shardsWithKnob);

shardsWithKnob.stop();

jsTest.log('Negative tests');

// Now we do negative tests to check that the message isn't logged if we don't set the parameter.
const standaloneWithoutKnob = MongoRunner.runMongod();
const standaloneWithoutKnobDB = standaloneWithoutKnob.getDB(dbName);

checkLogs(standaloneWithoutKnobDB, deprecationMessage, 0);

MongoRunner.stopMongod(standaloneWithoutKnob);

const shardsWithoutKnob = new ShardingTest({
    shards: 2,
    mongos: 1,
});
const sessionWithoutKnob = shardsWithoutKnob.s.getDB(dbName).getMongo().startSession();
const shardedWithoutKnobDB = sessionWithoutKnob.getDatabase(dbName);

checkLogs(shardedWithoutKnobDB, deprecationMessage, 0);
checkLogs(shardsWithoutKnob.shard0.getDB(shardedWithoutKnobDB), deprecationMessage, 0);
checkLogs(shardsWithoutKnob.shard1.getDB(shardedWithoutKnobDB), deprecationMessage, 0);

shardsWithoutKnob.stop();
})();