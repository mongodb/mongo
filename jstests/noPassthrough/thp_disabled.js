/**
 * Tests that the mongod and mongos processes are running with thp disabled.
 */

import {findMatchingLogLine} from "jstests/libs/log.js";
import {isLinux} from "jstests/libs/os_helpers.js";

function testMongod() {
    const conn = MongoRunner.runMongod();
    const adminDb = conn.getDB("admin");

    const mongodLog = assert.commandWorked(adminDb.runCommand({getLog: "global"}));
    const line = findMatchingLogLine(mongodLog.log,
                                     {msg: "Successfully disabled THP on mongod", id: 8751801});
    assert(line);

    MongoRunner.stopMongod(conn);
}

function testMongos() {
    let st = new ShardingTest({shards: 1});

    const mongosLog = assert.commandWorked(st.s.adminCommand({getLog: "global"}));
    const line = findMatchingLogLine(mongosLog.log,
                                     {msg: "Successfully disabled THP on mongos", id: 8751803});
    assert(line);

    st.stop();
}

if (!isLinux()) {
    jsTestLog("THP is only available on a linux platform.");
    quit();
}

testMongod();
testMongos();
