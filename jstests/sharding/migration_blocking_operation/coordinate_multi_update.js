/*
 * Basic tests for _shardsvrCoordinateMultiUpdate.
 * @tags: [
 *  requires_fcv_80
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: {rs0: {nodes: 3}}});
const replicaSet = new ReplSetTest({nodes: 1});
replicaSet.startSet();
replicaSet.initiate();

const dbName = "test";
const collName = "coll";
const namespace = `${dbName}.${collName}`;

assert.commandWorked(
    st.s0
        .getDB(dbName)
        .getCollection(collName)
        .insertMany([
            {
                _id: 1,
                member: "abc123",
                points: 0,
            },
            {
                _id: 2,
                member: "abc123",
                points: 59,
            },
        ]),
);

const databaseVersion = assert.commandWorked(st.s.adminCommand({getDatabaseVersion: dbName})).dbVersion;

function assertCoordinateMultiUpdateReturns(connection, code) {
    const response = connection.adminCommand({
        _shardsvrCoordinateMultiUpdate: namespace,
        uuid: UUID(),
        databaseVersion,
        command: {
            update: collName,
            updates: [{q: {member: "abc123"}, u: {$set: {points: 50}}, multi: true}],
        },
    });
    if (code === ErrorCodes.OK) {
        const res = assert.commandWorked(response);
        const underlyingUpdateResult = res["result"];
        assert.eq(underlyingUpdateResult["nModified"], 2);
        assert.eq(underlyingUpdateResult["n"], 2);
        assert.eq(underlyingUpdateResult["ok"], 1);
    } else {
        assert.commandFailedWithCode(response, code);
    }
}

// Command runs successfully on shard server.
assertCoordinateMultiUpdateReturns(st.rs0.getPrimary(), ErrorCodes.OK);
// Verify _shardsvrCoordinateMultiUpdate only runs on shard servers.
assertCoordinateMultiUpdateReturns(st.rs0.getSecondary(), ErrorCodes.NotWritablePrimary);
assertCoordinateMultiUpdateReturns(st.s, ErrorCodes.CommandNotFound);
assertCoordinateMultiUpdateReturns(replicaSet.getPrimary(), ErrorCodes.ShardingStateNotInitialized);

replicaSet.stopSet();
st.stop();
