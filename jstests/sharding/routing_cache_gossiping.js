/**
 * Test shards gossip back routing cache versions when requested to.
 *
 * @tags: [
 *   featureFlagShardedAggregationCatalogCacheGossiping,
 *   requires_fcv_80,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

// This test requires running commands directly against the shard since 'requestGossipRoutingCache'
// is a shard-only field.
TestData.replicaSetEndpointIncompatible = true;

const st = new ShardingTest({shards: 1});

const dbName = "test";

const collName1 = "foo1";
const ns1 = dbName + "." + collName1;

const collName2 = "foo2";
const ns2 = dbName + "." + collName2;

const nonexistentNs = dbName + ".nonexistent";

// Runs an aggregation on connection 'conn', requesting routing table gossiping for the namespaces
// specified on array 'requestGossipCollections'. Returns 'routingCacheGossip' field of the command
// response, which is of form [{nss: foo, collectionVersion: x}, ...].
function runCommandRequestingGossiping(conn, requestGossipCollections) {
    let response = assert.commandWorked(conn.getDB(dbName).runCommand({
        aggregate: collName1,
        pipeline: [],
        cursor: {},
        requestGossipRoutingCache: requestGossipCollections
    }));

    return response['routingCacheGossip'];
}

// Given a 'routingCacheGossip' response array, find the collectionVersion corresponding to
// namespace nss. Return undefined if not found.
function getGossipedVersion(gossipResponseArray, nss) {
    let gossipElem = gossipResponseArray.find((element) => element.nss === nss);
    return gossipElem ? gossipElem.collectionVersion : undefined;
}

function getExpectedCollectionVersion(nss) {
    let shardMetadata;
    assert.soon(() => {
        shardMetadata = ShardVersioningUtil.getMetadataOnShard(st.shard0, nss);
        return bsonWoCompare(shardMetadata, {});
    });
    return {
        e: shardMetadata.collVersionEpoch,
        t: shardMetadata.collVersionTimestamp,
        v: shardMetadata.collVersion
    };
}

assert.commandWorked(st.s.adminCommand({shardCollection: ns1, key: {x: 'hashed'}}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns2, key: {x: 'hashed'}}));

const ns1CollectionVersion = getExpectedCollectionVersion(ns1);
const ns2CollectionVersion = getExpectedCollectionVersion(ns2);

// Make sure the shard has refreshed it's routing cache for ns1 and ns2.
{
    const db = st.s.getDB(dbName);
    const coll1 = db[collName1];
    const coll2 = db[collName2];

    assert.commandWorked(coll1.insert({x: 1}));
    assert.commandWorked(coll2.insert({x: 1}));

    // Run $lookup with each namespace as secondary collection. The shard will need to route for
    // that collection, so it will refresh its routing cache.
    coll1.aggregate(
        [{$lookup: {from: coll2.getName(), localField: 'x', foreignField: 'x', as: 'out'}}]);
    coll2.aggregate(
        [{$lookup: {from: coll1.getName(), localField: 'x', foreignField: 'x', as: 'out'}}]);
}

// Check that when no gossip is requested to the shard, then the shard does not gossip back
// anything.
{
    let gossipOut = runCommandRequestingGossiping(st.shard0, []);
    assert.eq(undefined, gossipOut);
}

// Check that when gossip is requested to the shard, then the shard gossips back the versions it
// knows of.
{
    let gossipOut = runCommandRequestingGossiping(st.shard0, [ns1, ns2, nonexistentNs]);

    // Expect shard to gossip back ns1 and ns2, but not nonexistentNs.
    assert.eq(2, gossipOut.length);

    assert.eq(ns1CollectionVersion, getGossipedVersion(gossipOut, ns1));
    assert.eq(ns2CollectionVersion, getGossipedVersion(gossipOut, ns2));
    assert.eq(undefined, getGossipedVersion(gossipOut, nonexistentNs));
}

// Make sure the shard does not refresh its cache in order to serve the gossip request. It just
// gossips back whatever was already in its cache.
{
    st.shard0.adminCommand({flushRouterConfig: ns2});
    let gossipOut = runCommandRequestingGossiping(st.shard0, [ns1, ns2]);

    // Expect shard to gossip back ns1, but not ns2 because it is not in its cache.
    assert.eq(1, gossipOut.length);
    assert.eq(ns1CollectionVersion, getGossipedVersion(gossipOut, ns1));
    assert.eq(undefined, getGossipedVersion(gossipOut, ns2));
}

// Pass 'requestGossipCollections' to a command sent to mongos. Expect it to be ignored, and nothing
// gossiped back to the client.
{
    let gossipOut = runCommandRequestingGossiping(st.s, [ns1, ns2]);
    assert.eq(undefined, gossipOut);
}

st.stop();
