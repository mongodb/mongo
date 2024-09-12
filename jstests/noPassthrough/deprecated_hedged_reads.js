/**
 * Hedged reads and opportunistic secondary targeting are deprecated as of 8.0.
 *
 * This test ensures that a log message is emitted when the client
 * explicitly specifies a hedging option in a command. The test also ensures
 * that log warnings are emitted when the server parameters `readHedgingMode`,
 * `maxTimeMSForHedgedReads`, or `opportunisticSecondaryTargeting` are set,
 * either at setup or at runtime.
 *
 * @tags: [
 *   requires_fcv_81,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    mongos: [{
        setParameter: {
            maxTimeMSForHedgedReads: 100,
            readHedgingMode: "on",
            opportunisticSecondaryTargeting: false,
        }
    }],
    shards: 1,
    config: 1
});
const collName = "test";

checkLog.containsWithCount(st.s, "readHedgingMode parameter has no effect", 1);
checkLog.containsWithCount(st.s, "maxTimeMSForHedgedReads parameter has no effect", 1);
checkLog.containsWithCount(st.s, "opportunisticSecondaryTargeting parameter has no effect", 1);

assert.commandWorked(st.s.getDB('test').runCommand(
    {count: collName, $readPreference: {mode: "nearest", hedge: {enabled: true}}}));
checkLog.containsWithCount(st.s, "Hedged reads have been deprecated", 1);

assert.commandWorked(st.s.adminCommand({clearLog: 'global'}));

assert.commandWorked(st.s.adminCommand({setParameter: 1, readHedgingMode: "off"}));
checkLog.containsWithCount(st.s, "readHedgingMode parameter has no effect", 1);

assert.commandWorked(st.s.adminCommand({setParameter: 1, maxTimeMSForHedgedReads: 150}));
checkLog.containsWithCount(st.s, "maxTimeMSForHedgedReads parameter has no effect", 1);

assert.commandWorked(st.s.adminCommand({setParameter: 1, opportunisticSecondaryTargeting: true}));
checkLog.containsWithCount(st.s, "opportunisticSecondaryTargeting parameter has no effect", 1);

st.stop();
