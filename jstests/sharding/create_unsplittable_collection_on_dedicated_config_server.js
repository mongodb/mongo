/**
 * Tests that it is not possible to create a user data collection on a
 * dedicated config server.
 *
 * The test ensures collection creation fails on a config server when the create
 * request was built for the config shard, but it was migrated to a dedicated
 * config server during the request processing.
 *
 * @tags: [
 *   requires_fcv_81,
 *   requires_2_or_more_shards,
 *   assumes_stable_shard_list
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardTransitionUtil} from "jstests/libs/shard_transition_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kDbName = "test1";

const st = new ShardingTest({shards: 3, configShard: true, other: {enableBalancer: true}});

function getShardNames() {
    return st.s.adminCommand({listShards: 1}).shards.map(shard => shard._id);
}

let shardNames = getShardNames();

assert(shardNames.includes("config"));

// config shard
const configShard = st.shard0;

assert.eq("config", configShard.shardName);

// Starts the createUnsplittableTestCollection and hangs on the beginning
// of the request processing, when dataShard is set already to "config".
let failpoint = configureFailPoint(st.s, 'hangCreateUnshardedCollection');

// Starting the parallel shell for createUnsplittableCollection
const awaitResult = startParallelShell(
    funWithArgs(function(host, shardName, dbName) {
        const mongos = new Mongo(host);
        assert.commandFailedWithCode(
            mongos.getDB(dbName).runCommand(
                {createUnsplittableCollection: "test1coll", dataShard: shardName}),
            ErrorCodes.ShardNotFound);
    }, st.s.host, st.shard0.shardName, kDbName), st.s.port);

// Run transitionToDedicatedConfigServer, so that the config shard is migrated to
// the dedicated config server.
ShardTransitionUtil.transitionToDedicatedConfigServer(st);

// Disable the failpoint, so that it attempts to create the collection on the dedicated
// config server, which must fail.
failpoint.off();
awaitResult();

jsTest.log(
    "Testing that creating an unsplittable collection on a non-existing shard or the config server fails.");
{
    const kDataColl = 'unsplittable_collection_on_config_shard';

    assert.commandFailedWithCode(
        st.s.getDB(kDbName).runCommand(
            {createUnsplittableCollection: kDataColl, dataShard: "config"}),
        ErrorCodes.ShardNotFound,
        'Collection coordinator should reject any attempt to create a collection on a dedicated config server');

    assert.commandFailedWithCode(
        st.s.getDB(kDbName).runCommand(
            {createUnsplittableCollection: kDataColl, dataShard: "non-existing-shard-name"}),
        ErrorCodes.ShardNotFound);
}

st.stop();
