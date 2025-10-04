/**
 * Basic integration tests for updateZoneKeyRange command on sharded, unsharded, and sharded with
 * compound shard key collections.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 1});

let configDB = st.s.getDB("config");
let shardName = configDB.shards.findOne()._id;

assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: "x"}));
assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));

let currentMinBoundary;
let currentMaxBoundary;
let currentZone;

function testZoneOnShard(ns, testParameters) {
    let chunkMinBoundary = testParameters["min"];
    let chunkMaxBoundary = testParameters["max"];
    let zone = testParameters["zone"];
    let errorCodes = testParameters["errorCodes"];

    if (errorCodes.length === 0) {
        assert.commandWorked(
            st.s.adminCommand({updateZoneKeyRange: ns, min: chunkMinBoundary, max: chunkMaxBoundary, zone: zone}),
        );
        let tagDoc = configDB.tags.findOne();
        if (zone === null) {
            // Testing basic remove
            assert.eq(null, tagDoc);
        } else {
            // Testing basic assign works
            currentMinBoundary = chunkMinBoundary;
            currentMaxBoundary = chunkMaxBoundary;
            currentZone = zone;
            verifyChunkBounds(tagDoc, ns, currentMinBoundary, currentMaxBoundary, currentZone);
        }
    } else {
        assert.commandFailedWithCode(
            st.s.adminCommand({updateZoneKeyRange: ns, min: chunkMinBoundary, max: chunkMaxBoundary, zone: zone}),
            errorCodes,
        );

        let tagDoc = configDB.tags.findOne();
        verifyChunkBounds(tagDoc, ns, currentMinBoundary, currentMaxBoundary, currentZone);
    }
}

function verifyChunkBounds(tagDoc, ns, minKey, maxKey, tag) {
    assert.eq(ns, tagDoc.ns);
    assert.eq(minKey, tagDoc.min);
    assert.eq(maxKey, tagDoc.max);
    assert.eq(tag, tagDoc.tag);
}

let basicIntegrationTestCases = [
    {"min": {x: 0}, "max": {x: 10}, "zone": "x", "errorCodes": []},
    {"min": {x: -10}, "max": {x: 20}, "zone": "x", "errorCodes": [ErrorCodes.RangeOverlapConflict]},
    {
        "min": {x: 10},
        "max": {x: 0},
        "zone": "x",
        // TODO SERVER-96757: remove FailedToParse error code once 9.0 becomes last LTS
        "errorCodes": [ErrorCodes.BadValue, ErrorCodes.FailedToParse],
    },
    {"min": {x: 0}, "max": {x: 10}, "zone": null, "errorCodes": []},
];

/**
 * Basic integration test for updateZoneKeyRange ensuring we can successfully set zone boundaries
 * min: {x:0}, max: {x:10}. Then goes through the following cases:
 *
 * Case 1:
 *  Fails to update zone key range that overlaps with existing zone
 *  min: {x: -10}, max: {x: 20}
 *
 * Case 2:
 *  Fails to update zone key range with invalid range min > max
 *  min: {x: 10}, max: {x: 0}
 *
 * Case 3:
 *  Successfully does basic remove of zone by setting zone to null
 *
 */

assert.commandWorked(st.s.adminCommand({shardCollection: "test.ranged", key: {x: 1}}));
basicIntegrationTestCases.forEach(function (test) {
    testZoneOnShard("test.ranged", test);
});

/**
 * Basic integration test for updateZoneKeyRange on an unsharded collection ensuring we can
 * successfully set zone boundaries min: {x:0}, max: {x:10}. Then goes through the same cases as
 * before
 */

basicIntegrationTestCases.forEach(function (test) {
    testZoneOnShard("test.unsharded", test);
});

/**
 * Basic integration test for updateZoneKeyRange on an sharded collection with a compound key
 * ensuring we can successfully set zone boundaries min: {_id: 0, x: 0}, max: {_id: 100, x: 10}.
 * Then goes through the following cases:
 *
 * Case 1:
 *  Fails to update zone key range with invalid second field in compound key range min > max
 *  min: {_id: 100, x: 10}, max: {_id: 100, x: 1}
 *
 * Case 2:
 *  Fails to update zone key range with invalid first field in compound key range min > max
 *  min: {_id: 10, x: 1}, max: {_id: 1, x: 10}
 *
 * Case 3:
 *  Fails to update zone key range with invalid both invalid fields in compound key range min > max
 *  min: {_id: 10, x: 10}, max: {_id: 1, x: 1}
 *
 * Case 4:
 *  Successfully does basic remove of zone by setting zone to null
 */

let compoundKeyTestCases = [
    {"min": {_id: 0, x: 0}, "max": {_id: 100, x: 10}, "zone": "x", "errorCodes": []},
    {
        "min": {_id: 100, x: 10},
        "max": {_id: 100, x: 1},
        "zone": "x",
        // TODO SERVER-96757: remove FailedToParse error code once 9.0 becomes last LTS
        "errorCodes": [ErrorCodes.BadValue, ErrorCodes.FailedToParse],
    },
    {
        "min": {_id: 10, x: 1},
        "max": {_id: 1, x: 10},
        "zone": "x",
        // TODO SERVER-96757: remove FailedToParse error code once 9.0 becomes last LTS
        "errorCodes": [ErrorCodes.BadValue, ErrorCodes.FailedToParse],
    },
    {
        "min": {_id: 10, x: 10},
        "max": {_id: 1, x: 1},
        "zone": "x",
        // TODO SERVER-96757: remove FailedToParse error code once 9.0 becomes last LTS
        "errorCodes": [ErrorCodes.BadValue, ErrorCodes.FailedToParse],
    },
    {
        "min": {_id: 0, x: 0},
        "max": {_id: 0, x: 0},
        "zone": "x",
        // TODO SERVER-96757: remove FailedToParse error code once 9.0 becomes last LTS
        "errorCodes": [ErrorCodes.BadValue, ErrorCodes.FailedToParse],
    },
    {"min": {_id: 0, x: 0}, "max": {_id: 100, x: 10}, "zone": null, "errorCodes": []},
];

assert.commandWorked(st.s.adminCommand({shardCollection: "test.compound", key: {_id: 1, x: 1}}));
compoundKeyTestCases.forEach(function (test) {
    testZoneOnShard("test.compound", test);
});

st.stop();
