/**
 * Ensure $out handles simultaneous movePrimary operation gracefully, and completes query execution.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

const mongosDB = st.s.getDB(jsTestName());
const sourceColl = mongosDB["source"];
const targetColl = mongosDB["target"];

function testFn(shardSourceColl) {
    sourceColl.drop();
    targetColl.drop();

    if (shardSourceColl) {
        assert.commandWorked(
            st.s.adminCommand({shardCollection: sourceColl.getFullName(), key: {_id: "hashed"}}));
    }

    assert.commandWorked(sourceColl.insert({_id: 0}));

    let outFp = configureFailPoint(st.shard0, "outWaitAfterTempCollectionCreation");

    assert.commandWorked(
        st.s.adminCommand({movePrimary: mongosDB.getName(), to: st.shard0.shardName}));

    let comment = jsTestName() + "_comment";
    let outFn = `
    const sourceDB = db.getSiblingDB(jsTestName());
    const sourceColl = sourceDB["${sourceColl.getName()}"];
    const targetColl = sourceDB["${targetColl.getName()}"];
    let cmdRes = sourceDB.runCommand({
        aggregate: "${sourceColl.getName()}",
        pipeline: [{ $out: "${targetColl.getName()}" }],
        cursor: {},
        comment: "${comment}"
    });

    // It is expected for the query to fail with QueryPlanKilled when reading from a collection
    // that gets moved by movePrimary. So by making the source collection sharded we ensure that
    // movePrimary does not move it – but still moves the output collection. So we test that
    // $out survives the output collection being moved.
    const mayFailWithQueryPlanKilled = !${shardSourceColl}
    if (mayFailWithQueryPlanKilled) {
        assert.commandWorkedOrFailedWithCode(cmdRes, ErrorCodes.QueryPlanKilled);
    } else {
        assert.commandWorked(cmdRes);
    }

    if (cmdRes.ok) {
        // Sanity check that if $out completed and we're able to read the contents of the output
        // collection.
        assert.eq(1, targetColl.find().itcount());
    }
`;

    let outShell = startParallelShell(outFn, st.s.port);

    outFp.wait();

    assert.commandWorked(
        st.s.adminCommand({movePrimary: mongosDB.getName(), to: st.shard1.shardName}));

    outFp.off();

    outShell();
}

testFn(false /* shardSourceColl */);
testFn(true /* shardSourceColl */);

st.stop();
