/**
 * Tests the behavior of lockContentionMetrics in the ServerStatus command.
 *
 * @tags: [
 *      # TODO(SERVER-110898): Remove once TSAN works with ObservableMutex.
 *      tsan_incompatible,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Constants for expected fields in the lockContentionMetrics response.
const kSampleFields = [
    "serviceContextMutex",
    "logv2CompositeBackendBackendTraitsMutex",
    "logv2RamLogGlobalMutex",
    "logv2RamLogStartupWarningsMutex",
];

// Constants for expected fields that register with an instanceLabel; only emitted when listAll is enabled.
// All tested topologies (mongos, mongod primary, mongod secondary) instantiate these networking
// components, so each mutex is expected to appear in every serverStatus response.
const kSampleInstanceLabelFields = [
    "connectionPoolMutex",
    "networkInterfaceTlMutex",
    "tlTypeFactoryMutex",
];

let checkNum = 1;

function runServerStatus(conn, options) {
    const cmdObj = {serverStatus: 1, ...options};
    return conn.getDB("admin").runCommand(cmdObj);
}

function runServerStatusAndAssert(conn, options, listAll) {
    const resp = runServerStatus(conn, options).lockContentionMetrics;
    jsTest.log.info(`Check ${checkNum++}: ` + tojson(resp));
    assert(resp);

    let errors = [];
    for (const field of kSampleFields) {
        const entry = resp[field];
        if (!entry) {
            errors.push(`Missing field: ${field}`);
            continue;
        }
        if (!entry["exclusive"]) {
            errors.push(`Missing 'exclusive' in ${field}`);
        }
        if (!entry["shared"]) {
            errors.push(`Missing 'shared' in ${field}`);
        }
        if ((entry["mutexes"] != undefined) !== listAll) {
            errors.push(`'mutexes' presence mismatch in ${field}`);
        }
    }

    if (listAll) {
        for (const field of kSampleInstanceLabelFields) {
            const entry = resp[field];
            if (!entry) {
                errors.push(`Missing field: ${field}`);
                continue;
            }
            if (
                !entry["mutexes"] ||
                !entry["mutexes"].every((m) => m["instanceLabel"] != undefined)
            ) {
                errors.push(`Missing mutex 'instanceLabel' in ${field}`);
            }
        }
    }

    assert.eq(errors, []);
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
