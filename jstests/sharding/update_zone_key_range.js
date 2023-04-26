/**
 * Basic integration tests for updateZoneKeyRange command on sharded, unsharded, and sharded with
 * compound shard key collections.
 */

(function() {
'use strict';

var st = new ShardingTest({shards: 1});

var configDB = st.s.getDB('config');
var shardName = configDB.shards.findOne()._id;

assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: 'x'}));
assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));

var currentMinBoundary;
var currentMaxBoundary;
var currentZone;

function testZoneOnShard(ns, testParameters) {
    var chunkMinBoundary = testParameters["min"];
    var chunkMaxBoundary = testParameters["max"];
    var zone = testParameters["zone"];
    var returnCode = testParameters["returnCode"];

    if (returnCode === 0) {
        assert.commandWorked(st.s.adminCommand(
            {updateZoneKeyRange: ns, min: chunkMinBoundary, max: chunkMaxBoundary, zone: zone}));
        var tagDoc = configDB.tags.findOne();
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
            st.s.adminCommand(
                {updateZoneKeyRange: ns, min: chunkMinBoundary, max: chunkMaxBoundary, zone: zone}),
            returnCode);

        var tagDoc = configDB.tags.findOne();
        verifyChunkBounds(tagDoc, ns, currentMinBoundary, currentMaxBoundary, currentZone);
    }
}

function verifyChunkBounds(tagDoc, ns, minKey, maxKey, tag) {
    assert.eq(ns, tagDoc.ns);
    assert.eq(minKey, tagDoc.min);
    assert.eq(maxKey, tagDoc.max);
    assert.eq(tag, tagDoc.tag);
}

var basicIntegrationTestCases = [
    {'min': {x: 0}, 'max': {x: 10}, 'zone': 'x', 'returnCode': 0},
    {'min': {x: -10}, 'max': {x: 20}, 'zone': 'x', 'returnCode': ErrorCodes.RangeOverlapConflict},
    {'min': {x: 10}, 'max': {x: 0}, 'zone': 'x', 'returnCode': ErrorCodes.FailedToParse},
    {'min': {x: 0}, 'max': {x: 10}, 'zone': null, 'returnCode': 0}
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

assert.commandWorked(st.s.adminCommand({shardCollection: 'test.ranged', key: {x: 1}}));
basicIntegrationTestCases.forEach(function(test) {
    testZoneOnShard('test.ranged', test);
});

/**
 * Basic integration test for updateZoneKeyRange on an unsharded collection ensuring we can
 * successfully set zone boundaries min: {x:0}, max: {x:10}. Then goes through the same cases as
 * before
 */

basicIntegrationTestCases.forEach(function(test) {
    testZoneOnShard('test.unsharded', test);
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

var compoundKeyTestCases = [
    {'min': {_id: 0, x: 0}, 'max': {_id: 100, x: 10}, 'zone': 'x', 'returnCode': 0},
    {
        'min': {_id: 100, x: 10},
        'max': {_id: 100, x: 1},
        'zone': 'x',
        'returnCode': ErrorCodes.FailedToParse
    },
    {
        'min': {_id: 10, x: 1},
        'max': {_id: 1, x: 10},
        'zone': 'x',
        'returnCode': ErrorCodes.FailedToParse
    },
    {
        'min': {_id: 10, x: 10},
        'max': {_id: 1, x: 1},
        'zone': 'x',
        'returnCode': ErrorCodes.FailedToParse
    },
    {
        'min': {_id: 0, x: 0},
        'max': {_id: 0, x: 0},
        'zone': 'x',
        'returnCode': ErrorCodes.FailedToParse
    },
    {'min': {_id: 0, x: 0}, 'max': {_id: 100, x: 10}, 'zone': null, 'returnCode': 0}
];

assert.commandWorked(st.s.adminCommand({shardCollection: 'test.compound', key: {_id: 1, x: 1}}));
compoundKeyTestCases.forEach(function(test) {
    testZoneOnShard('test.compound', test);
});

st.stop();
})();
