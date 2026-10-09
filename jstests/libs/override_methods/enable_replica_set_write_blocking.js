/**
 * Enables replica set write blocking on one shard in an authenticated passthrough suite.
 *
 * blockReplicaSetWrites is registered on shard servers, so the command must be sent directly to
 * the selected shard replica set primary instead of through mongos.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const keyFile = "jstests/libs/authTestsKey";
const replicaSetHost = db.getMongo().isMongos()
    ? db.getSiblingDB("config").shards.find().sort({_id: 1}).next()?.host
    : db.getMongo().host;
assert(replicaSetHost, "The passthrough fixture must contain a replica set");

const replicaSet = new ReplSetTest(replicaSetHost);
const primary = replicaSet.getPrimary();
replicaSet.asCluster(
    primary,
    () => {
        assert.commandWorked(
            primary.getDB("admin").runCommand({
                blockReplicaSetWrites: 1,
                enabled: true,
                allowDeletions: false,
                reason: "InsufficientDiskSpace",
            }),
        );
    },
    keyFile,
);
