/*
 * Ensure that `killAllSessions` fails when killing sessions on
 * either (a) one of the shards or (b) all of the shards
 * fails, and verify the log output.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    setFailCommandOnShards,
    unsetFailCommandOnEachShard,
} from "jstests/sharding/libs/sharded_transactions_helpers.js";

function runTest({st, numShards, closeConnection}) {
    const hostArray =
        Array.from({length: numShards}, (_, i) => st["rs" + i].getPrimary().host).sort();

    const data = {
        errorCode: ErrorCodes.CommandFailed,
        failCommands: ["killAllSessionsByPattern"],
        closeConnection: closeConnection
    };
    setFailCommandOnShards(st, "alwaysOn", data, numShards);

    assert.commandFailed(st.s.adminCommand({"killAllSessions": []}));
    const relevantLog = checkLog.getFilteredLogMessages(st.s, 8963000, {});
    assert.eq(relevantLog.length, 1, relevantLog);

    // Ensure that the `failedHosts` attribute of the log includes the
    // host(s) that were programmed to fail a `killAllSessions` command.
    assert.eq(relevantLog[0].attr.failedHosts.sort(), hostArray, relevantLog);

    unsetFailCommandOnEachShard(st, numShards);
    assert.commandWorked(st.s.adminCommand({clearLog: 'global'}));
}

const st = new ShardingTest({mongos: 1, rs: {nodes: 1}, shards: 2});

runTest({st: st, numShards: 1, closeConnection: false});
runTest({st: st, numShards: 1, closeConnection: true});
runTest({st: st, numShards: 2, closeConnection: false});
runTest({st: st, numShards: 2, closeConnection: true});

st.stop();
