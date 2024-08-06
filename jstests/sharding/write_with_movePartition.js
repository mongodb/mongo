/**
 * Ensure $out handles simultaneous movePrimary operation gracefully, and completes query execution.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

const mongosDB = st.s.getDB(jsTestName());
const sourceColl = mongosDB["source"];
const targetColl = mongosDB["target"];

assert.commandWorked(sourceColl.insert({_id: 0}));

let outFp = configureFailPoint(st.shard0, "outWaitAfterTempCollectionCreation");

assert.commandWorked(st.s.adminCommand({movePrimary: mongosDB.getName(), to: st.shard0.shardName}));

let comment = jsTestName() + "_comment";
let outFn = `
    const sourceDB = db.getSiblingDB(jsTestName());
    const sourceColl = sourceDB["${sourceColl.getName()}"];
    let cmdRes = sourceDB.runCommand({
        aggregate: "${sourceColl.getName()}",
        pipeline: [{ $out: "${targetColl.getName()}" }],
        cursor: {},
        comment: "${comment}"
    });
    assert.commandWorked(cmdRes);
`;

let outShell = startParallelShell(outFn, st.s.port);

outFp.wait();

assert.commandWorked(st.s.adminCommand({movePrimary: mongosDB.getName(), to: st.shard1.shardName}));

outFp.off();

outShell();
// Sanity check that the $out completed and we're able to read the contents of the output
// collection.
assert.eq(1, targetColl.find().itcount());
st.stop();
