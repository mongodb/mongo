/**
 * Verifies that the mongos errors when starting a new transaction at shutdown.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 1, mongos: 1});

const pauseAfterImplicitlyAbortAllTransactionsFp =
    configureFailPoint(st.s0, "pauseAfterImplicitlyAbortAllTransactions");

st.stopMongos(0, {}, {waitpid: false});
pauseAfterImplicitlyAbortAllTransactionsFp.wait();

const session = st.s0.startSession();
session.startTransaction();
assert.commandFailedWithCode(session.getDatabase("testDB")["testColl"].insert({x: 2}),
                             ErrorCodes.HostUnreachable);

pauseAfterImplicitlyAbortAllTransactionsFp.off();

st.restartMongos(0);
st.stop();
