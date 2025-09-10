/**
 * Tests the behavior of lockContentionMetrics in the ServerStatus command.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Constant for expected field in the lockContentionMetrics response.
const kSampleField = "ServiceContext::_mutex";
let checkNum = 1;

function runServerStatus(conn, options) {
    const cmdObj = {serverStatus: 1, ...options};
    return conn.getDB("admin").runCommand(cmdObj);
}

function runServerStatusAndAssert(conn, options, listAll) {
    const resp = runServerStatus(conn, options).lockContentionMetrics;
    jsTest.log.info(`Check ${checkNum++}: ` + tojson(resp));
    assert(resp);

    const entry = resp[kSampleField];
    assert(entry);
    assert(entry["exclusive"]);
    assert(entry["shared"]);
    assert.eq(entry["mutexes"] != undefined, listAll);
}

function runTest(conn) {
    // listAll should default to false if not supplied or supplied incorrectly.
    runServerStatusAndAssert(conn, {lockContentionMetrics: 1}, false);
    runServerStatusAndAssert(conn, {lockContentionMetrics: {list: 1}}, false);

    runServerStatusAndAssert(conn, {lockContentionMetrics: {listAll: 1}}, true);
    runServerStatusAndAssert(conn, {lockContentionMetrics: {listAll: 0}}, false);

    // lockContentionMetrics should not emit when not explicitly included in command body.
    assert.eq(runServerStatus(conn).lockContentionMetrics, undefined);
}

const st = new ShardingTest({shards: 1, rs: {nodes: 2}, mongos: 1});
runTest(st.s0);
runTest(st.rs0.getPrimary());
runTest(st.rs0.getSecondaries()[0]);
st.stop();
