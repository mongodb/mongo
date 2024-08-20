/**
 * Checks that cluster server parameters are valid for storage in update/upsert
 *
 * @tags: [
 *  ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(conn) {
    let db = conn.getDB("admin");

    assert.commandFailedWithCode(db.adminCommand({
        setClusterParameter: {fleCompactionOptions: {maxCompactionSize: NumberInt(1), $zip: 1}}
    }),
                                 ErrorCodes.DollarPrefixedFieldName);

    assert.commandWorked(db.adminCommand(
        {setClusterParameter: {fleCompactionOptions: {maxCompactionSize: NumberInt(1)}}}));

    assert.commandFailedWithCode(db.adminCommand({
        setClusterParameter: {fleCompactionOptions: {maxCompactionSize: NumberInt(1), $zip: 1}}
    }),
                                 ErrorCodes.DollarPrefixedFieldName);
}

jsTestLog("Standalone: Testing setClusterParameter");
{
    const mongo = MongoRunner.runMongod();

    runTest(mongo);

    MongoRunner.stopMongod(mongo);
}

jsTestLog("ReplicaSet: Testing setClusterParameter");
{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();

    rst.initiate();
    rst.awaitReplication();

    runTest(rst.getPrimary());

    rst.stopSet();
}

jsTestLog("Sharding: Testing setClusterParameter");
{
    let options = {
        mongos: 1,
        config: 1,
        shards: 2,
        rs: {
            nodes: 1,
        },
    };

    let st = new ShardingTest(options);
    runTest(st.s);
    st.stop();
}
