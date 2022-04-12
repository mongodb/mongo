/**
 * Tests the behavior of a $graphLookup on a sharded 'from' collection in various situations. These
 * include when the local collection is sharded and unsharded, when the $graphLookup can target
 * shards or is scatter-gather, and when the $graphLookup is not top-level.
 *
 * @tags: [requires_fcv_51]
 */

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq.
load("jstests/libs/profiler.js");             // For profilerHas*OrThrow helper functions.

const st = new ShardingTest({shards: 2, mongos: 1});
const testName = "sharded_graph_lookup";

const mongosDB = st.s0.getDB(testName);
const shardList = [st.shard0.getDB(testName), st.shard1.getDB(testName)];

assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.shard0.shardName);

// Turn on the profiler for both shards.
assert.commandWorked(st.shard0.getDB(testName).setProfilingLevel(2));
assert.commandWorked(st.shard1.getDB(testName).setProfilingLevel(2));

const airportsColl = mongosDB.airports;
const travelersColl = mongosDB.travelers;
const airfieldsColl = mongosDB.airfields;

function assertGraphLookupExecution(pipeline, opts, expectedResults, executionList) {
    assert.commandWorked(travelersColl.insert([
        {"_id": 1, "firstName": "Alice", "nearestAirport": "LHR"},
        {"_id": 2, "firstName": "Alice", "nearestAirport": "ORD"},
        {"_id": 3, "firstName": "Bob", "nearestAirport": "JFK"},
    ]));
    assert.commandWorked(airportsColl.insert([
        {"_id": 1, "airport": "BOS", "connects": "JFK", "country": "US"},
        {"_id": 2, "airport": "JFK", "connects": "LHR", "country": "US"},
        {"_id": 3, "airport": "LHR", "connects": "ORD", "country": "UK"},
        {"_id": 4, "airport": "ORD", "connects": "PWM", "country": "US"},
        {"_id": 5, "airport": "PWM", "connects": "BOS", "country": "US"},
    ]));
    assert.commandWorked(airfieldsColl.insert([
        {"_id": 1, "airfield": "LHR", "connects": "MIA"},
        {"_id": 2, "airfield": "JFK", "connects": "MIA"},
        {"_id": 3, "airfield": "ORD", "connects": "MIA"},
        {"_id": 4, "airfield": "MIA", "connects": "LHR"},
    ]));

    assert(arrayEq(expectedResults, travelersColl.aggregate(pipeline, opts).toArray()));

    for (let exec of executionList) {
        const collName = exec.collName ? exec.collName : travelersColl.getName();
        const fromCollName = exec.fromCollName ? exec.fromCollName : airportsColl.getName();
        const isLookup = exec.hasOwnProperty('subpipelineExec');

        // If the primary delegates the $graphLookup merging functionality to a random shard,
        // confirm the expected behavior here.
        if (exec.randomlyDelegatedMerger) {
            let totalGraphLookupExecution =
                shardList.reduce((numExecs, shard) => numExecs +
                                     shard.system.profile
                                         .find({
                                             "command.aggregate": collName,
                                             "command.comment": opts.comment,
                                             "command.pipeline.$mergeCursors": {$exists: true},
                                             "command.pipeline.$graphLookup": {$exists: true}
                                         })
                                         .itcount(),
                                 0);
            assert.eq(totalGraphLookupExecution, 1);
        }

        for (let shard = 0; shard < shardList.length; shard++) {
            // Confirm that top-level execution is as expected. In the nested cases, the top level
            // is a $lookup, so we check either for $lookup or $graphLookup as appropriate.
            if (!exec.randomlyDelegatedMerger) {
                profilerHasNumMatchingEntriesOrThrow({
                    profileDB: shardList[shard],
                    filter: {
                        "command.aggregate": collName,
                        "command.comment": opts.comment,
                        "command.pipeline.$graphLookup": {$exists: !isLookup},
                        "command.pipeline.$lookup": {$exists: isLookup},
                    },
                    numExpectedMatches: exec.toplevelExec[shard]
                });
            }

            // Confirm that the $graphLookup recursive $match execution is as expected. In the
            // nested cases, we need to check the $lookup subpipeline execution instead. In either
            // case, the command dispatched is an aggregate.
            profilerHasNumMatchingEntriesOrThrow({
                profileDB: shardList[shard],
                filter: {
                    "command.aggregate": fromCollName,
                    "command.comment": opts.comment,
                    "command.fromMongos": exec.mongosMerger === true
                },
                numExpectedMatches: isLookup ? exec.subpipelineExec[shard]
                                             : exec.recursiveMatchExec[shard]
            });
        }
    }

    assert(travelersColl.drop());
    assert(airportsColl.drop());
    assert(airfieldsColl.drop());
}

// Test unsharded local collection and sharded foreign collection, with a targeted $graphLookup.
st.shardColl(airportsColl, {airport: 1}, {airport: "LHR"}, {airport: "LHR"}, mongosDB.getName());
let pipeline = [
    {$graphLookup: {
        from: "airports",
        startWith: "$nearestAirport",
        connectFromField: "connects",
        connectToField: "airport",
        maxDepth: 1,
        as: "destinations"
    }},
    {$project: {firstName: 1, 'destinations.airport': 1}}
];
let expectedRes = [
    {
        _id: 1,
        firstName: "Alice",
        destinations: [{airport: "LHR"}, {airport: "ORD"}],
    },
    {
        _id: 2,
        firstName: "Alice",
        destinations: [{airport: "ORD"}, {airport: "PWM"}],
    },
    {
        _id: 3,
        firstName: "Bob",
        destinations: [{airport: "JFK"}, {airport: "LHR"}],
    }
];
assertGraphLookupExecution(
    pipeline, {comment: "unsharded_to_sharded_targeted"}, expectedRes, [{
        // Because the local collection is unsharded, the $graphLookup stage is executed on the
        // primary shard of the database.
        toplevelExec: [1, 0],
        // For every document that flows through the $graphLookup stage, the node executing it will
        // target the shard that holds the relevant data for the sharded foreign collection. Note:
        // Since $graphLookup maintains a cache of foreign documents, there is only one query per
        // unique document that we need to look up in the foreign collection.
        recursiveMatchExec: [1, 3]
    }]);

// Test unsharded local collection and sharded foreign collection, with an untargeted $graphLookup.
st.shardColl(airportsColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
assertGraphLookupExecution(pipeline, {comment: "unsharded_to_sharded_scatter"}, expectedRes, [{
                               // Because the local collection is unsharded, the $graphLookup stage
                               // is executed on the primary shard of the database.
                               toplevelExec: [1, 0],
                               // For every document that flows through the $graphLookup stage, the
                               // node executing it will perform a scatter-gather query and open a
                               // cursor on every shard that contains the foreign collection.
                               recursiveMatchExec: [4, 4]
                           }]);

// Test sharded local collection and sharded foreign collection, with a targeted $graphLookup.
st.shardColl(airportsColl, {airport: 1}, {airport: "LHR"}, {airport: "LHR"}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
assertGraphLookupExecution(
    pipeline, {comment: "sharded_to_sharded_targeted"}, expectedRes, [{
        // The 'travelers' collection is sharded, so the $graphLookup stage is executed in parallel
        // on every shard that contains the local collection.
        toplevelExec: [1, 1],
        // Each node executing the $graphLookup will, for every document that flows through the
        // stage, target the shard(s) that holds the relevant data for the sharded foreign
        // collection. The parallel $graphLookups do not share the same cache, so 'ORD' does not
        // need to be queried for twice, but 'LHR' does.
        recursiveMatchExec: [1, 4]
    }]);

// Test sharded local collection and sharded foreign collection, with an untargeted $graphLookup.
st.shardColl(airportsColl, {_id: 1}, {_id: 3}, {_id: 3}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
assertGraphLookupExecution(
    pipeline, {comment: "sharded_to_sharded_untargeted"}, expectedRes, [{
        // The 'travelers' collection is sharded, so the $graphLookup stage is executed in parallel
        // on every shard that contains the local collection.
        toplevelExec: [1, 1],
        // Each node executing the $graphLookup will, for every document that flows through it,
        // perform a scatter-gather query and open a cursor on every shard that contains the foreign
        // collection.
        recursiveMatchExec: [5, 5]
    }]);

// Test sharded local collection and sharded foreign collection, with a targeted $graphLookup
// including a 'restrictSearchWithMatch' field.
st.shardColl(airportsColl, {airport: 1}, {airport: "LHR"}, {airport: "LHR"}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
pipeline = [
    {$graphLookup: {
        from: "airports",
        startWith: "$nearestAirport",
        connectFromField: "connects",
        connectToField: "airport",
        maxDepth: 1,
        as: "destinations",
        restrictSearchWithMatch: {
            "country": "US"
        }
    }},
    {$project: {firstName: 1, 'destinations.airport': 1}}
];
expectedRes = [
    {
        _id: 1,
        firstName: "Alice",
        destinations: [],
    },
    {
        _id: 2,
        firstName: "Alice",
        destinations: [{airport: "ORD"}, {airport: "PWM"}],
    },
    {
        _id: 3,
        firstName: "Bob",
        destinations: [{airport: "JFK"}],
    }
];
assertGraphLookupExecution(
    pipeline, {comment: "sharded_to_sharded_targeted_restricted"}, expectedRes, [{
        // The 'travelers' collection is sharded, so the $graphLookup stage is executed in parallel
        // on every shard that contains the local collection.
        toplevelExec: [1, 1],
        // As in the sharded_to_sharded_targeted case, each node targets the shard(s) that holds the
        // relevant data. As expected, there are queries from both parallel $graphLookup executions
        // for the LHR airport, and these are filtered out by the 'restrictSearchWithMatch' filter.
        recursiveMatchExec: [1, 4]
    }]);

// Test sharded local collection and sharded foreign collection with a targeted top-level $lookup
// and a nested $graphLookup on an unsharded foreign collection.
st.shardColl(airportsColl, {airport: 1}, {airport: "JFK"}, {airport: "JFK"}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
pipeline = [
    {$lookup: {
        from: "airports",
        localField: "nearestAirport",
        foreignField: "airport",
        pipeline: [
            {$graphLookup: {
                from: "airfields",
                startWith: "$airport",
                connectFromField: "connects",
                connectToField: "airfield",
                maxDepth: 1,
                as: "nearbyAirfields"
            }},
            {$project: {'airport': 1, 'nearbyAirfields.airfield': 1, _id: 0}}
        ],
        as: "destinations"
    }},
    {$unwind: "$destinations"},
    {$project: {firstName: 1, 'destinations.airport' : 1, 'destinations.nearbyAirfields' : 1}}
];
expectedRes = [
    {
        _id: 1,
        firstName: "Alice",
        destinations: {airport: "LHR", nearbyAirfields: [{airfield: "LHR"}, {airfield: "MIA"}]},
    },
    {
        _id: 2,
        firstName: "Alice",
        destinations: {airport: "ORD", nearbyAirfields: [{airfield: "ORD"}, {airfield: "MIA"}]},
    },
    {
        _id: 3,
        firstName: "Bob",
        destinations: {airport: "JFK", nearbyAirfields: [{airfield: "JFK"}, {airfield: "MIA"}]},
    }
];
assertGraphLookupExecution(pipeline, {comment: "sharded_to_sharded_to_unsharded"}, expectedRes, [
    {
        // The 'travelers' collection is sharded, so the $lookup stage is executed in parallel on
        // every shard that contains the local collection.
        toplevelExec: [1, 1],
        // Each node executing the $lookup will, for every document that flows through the $lookup
        // stage, target the shard that holds the relevant data for the sharded foreign collection.
        subpipelineExec: [0, 3],

    },
    {
        collName: airportsColl.getName(),
        fromCollName: airfieldsColl.getName(),
        // When executing the subpipeline, the nested $graphLookup stage will stay on the merging
        // half of the pipeline and execute on the merging node, sending requests to execute the
        // nested $matches on the primary shard (where the unsharded 'airfields' collection is).
        // Only the $graphLookup on the non-primary shard needs to send requests over the network;
        // the rest can be done via a local read and are not logged. The $graphLookups cannot share
        // a cache because they run indepedently.
        toplevelExec: [0, 0],
        recursiveMatchExec: [2, 0]
    }
]);

// Test sharded local collection and sharded foreign collection with a targeted top-level $lookup
// and a nested targeted $graphLookup on a sharded foreign collection.
st.shardColl(airportsColl, {airport: 1}, {airport: "JFK"}, {airport: "JFK"}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
st.shardColl(
    airfieldsColl, {airfield: 1}, {airfield: "LHR"}, {airfield: "LHR"}, mongosDB.getName());
assertGraphLookupExecution(
    pipeline, {comment: "sharded_to_sharded_to_sharded_targeted"}, expectedRes, [
        {
            // The 'travelers' collection is sharded, so the $lookup stage is executed in parallel
            // on every shard that contains the local collection.
            toplevelExec: [1, 1],
            // Each node executing the $lookup will, for every document that flows through the
            // $lookup stage, target the shard that holds the relevant data for the sharded foreign
            // collection.
            subpipelineExec: [0, 3],
        },
        {
            collName: airportsColl.getName(),
            fromCollName: airfieldsColl.getName(),
            // When executing the subpipeline, the nested $graphLookup stage will stay on the
            // merging half of the pipeline and execute on the merging node, targeting shards to
            // execute the nested $matches.
            toplevelExec: [0, 0],
            recursiveMatchExec: [1, 5]
        }
    ]);

// Test sharded local collection and sharded foreign collection with a targeted top-level $lookup
// and a nested untargeted $graphLookup on a sharded foreign collection.
st.shardColl(airportsColl, {airport: 1}, {airport: "JFK"}, {airport: "JFK"}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
st.shardColl(airfieldsColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
assertGraphLookupExecution(
    pipeline, {comment: "sharded_to_sharded_to_sharded_scatter"}, expectedRes, [
        {
            // The 'travelers' collection is sharded, so the $lookup stage is executed in parallel
            // on every shard that contains the local collection.
            toplevelExec: [1, 1],
            // Each node executing the $lookup will, for every document that flows through the
            // $lookup stage, target the shard that holds the relevant data for the sharded foreign
            // collection.
            subpipelineExec: [0, 3],
        },
        {
            collName: airportsColl.getName(),
            fromCollName: airfieldsColl.getName(),
            // When executing the subpipeline, the nested $graphLookup stage will stay on the
            // merging half of the pipeline and execute on the merging node, performing a
            // scatter-gather query to execute the nested $matches.
            toplevelExec: [0, 0],
            recursiveMatchExec: [6, 6]
        }
    ]);

// Test sharded local collection where the foreign namespace is a sharded view with another
// $graphLookup against a sharded collection. Note that the $graphLookup in the view should be
// treated as a "nested" $graphLookup and should execute on the merging node.
st.shardColl(airportsColl, {airport: 1}, {airport: "JFK"}, {airport: "JFK"}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
st.shardColl(
    airfieldsColl, {airfield: 1}, {airfield: "LHR"}, {airfield: "LHR"}, mongosDB.getName());

assert.commandWorked(mongosDB.createView("airportsView", airportsColl.getName(),
    [{$graphLookup: {
        from: "airfields",
        startWith: "$airport",
        connectFromField: "connects",
        connectToField: "airfield",
        maxDepth: 1,
        as: "nearbyAirfields"
    }}]
));
pipeline = [
    {$graphLookup: {
        from: "airportsView",
        startWith: "$nearestAirport",
        connectFromField: "connects",
        connectToField: "airport",
        maxDepth: 0,
        as: "destinations"
    }},
    {$unwind: "$destinations"},
    {$project: {firstName: 1, 'destinations.airport' : 1, 'destinations.nearbyAirfields.airfield' : 1}}
];

assertGraphLookupExecution(pipeline, {comment: "sharded_to_sharded_view_to_sharded"}, expectedRes, [
    {
        // The 'travelers' collection is sharded, so the $graphLookup stage is executed in parallel
        // on every shard that contains the local collection.
        toplevelExec: [1, 1],
        // Each node executing the $graphLookup will, for every document that flows through the
        // stage, target the shard(s) that holds the relevant data for the sharded foreign view.
        recursiveMatchExec: [0, 3],
    },
    {
        collName: airportsColl.getName(),
        fromCollName: airfieldsColl.getName(),
        // When executing the subpipeline, the "nested" $graphLookup stage contained in the view
        // pipeline will stay on the merging half of the pipeline and execute on the merging node,
        // targeting shards to execute the nested $matches.
        toplevelExec: [0, 0],
        recursiveMatchExec: [1, 5]
    }
]);
mongosDB.airportsView.drop();

// Test top-level $lookup on a sharded local collection where the foreign namespace is a sharded
// view with a $graphLookup against a sharded collection. Note that the $graphLookup in the view
// should be treated as a "nested" $graphLookup and should execute on the merging node.
st.shardColl(airportsColl, {airport: 1}, {airport: "JFK"}, {airport: "JFK"}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
st.shardColl(
    airfieldsColl, {airfield: 1}, {airfield: "LHR"}, {airfield: "LHR"}, mongosDB.getName());

assert.commandWorked(mongosDB.createView("airportsView", airportsColl.getName(),
    [{$graphLookup: {
        from: "airfields",
        startWith: "$airport",
        connectFromField: "connects",
        connectToField: "airfield",
        maxDepth: 1,
        as: "nearbyAirfields"
    }}]
));
pipeline = [
    {$lookup: {
        from: "airportsView",
        localField: "nearestAirport",
        foreignField: "airport",
        as: "destinations"
    }},
    {$unwind: "$destinations"},
    {$project: {firstName: 1, 'destinations.airport' : 1, 'destinations.nearbyAirfields.airfield' : 1}}
];

assertGraphLookupExecution(
    pipeline, {comment: "sharded_lookup_to_sharded_view_to_sharded"}, expectedRes, [
        {
            // The 'travelers' collection is sharded, but mongos does not know that the foreign
            // namespace is a view on a sharded collection. It is instead treated as an unsharded
            // collection, and the top-level $lookup is only on the primary.
            toplevelExec: [1, 0],
            // Each node executing the $lookup will, for every document that flows through the stage
            // target the shard(s) that holds the relevant data for the sharded foreign view.
            subpipelineExec: [0, 3],
        },
        {
            collName: airportsColl.getName(),
            fromCollName: airfieldsColl.getName(),
            // When executing the subpipeline, the "nested" $graphLookup stage contained in the view
            // pipeline will stay on the merging half of the pipeline and execute on the merging
            // node, targeting shards to execute the nested $matches.
            toplevelExec: [0, 0],
            recursiveMatchExec: [1, 5]
        }
    ]);
mongosDB.airportsView.drop();

// Test sharded local collection where the foreign namespace is a sharded view with a $lookup
// against a sharded collection. Note that the $lookup in the view should be treated as a "nested"
// $lookup and should execute on the merging node.
st.shardColl(airportsColl, {airport: 1}, {airport: "JFK"}, {airport: "JFK"}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
st.shardColl(
    airfieldsColl, {airfield: 1}, {airfield: "LHR"}, {airfield: "LHR"}, mongosDB.getName());

assert.commandWorked(mongosDB.createView("airportsView", airportsColl.getName(),
    [{$lookup: {
        from: "airfields",
        localField: "airport",
        foreignField: "airfield",
        as: "nearbyAirfields"
    }}]
));
pipeline = [
    {$graphLookup: {
        from: "airportsView",
        startWith: "$nearestAirport",
        connectFromField: "connects",
        connectToField: "airport",
        maxDepth: 0,
        as: "destinations"
    }},
    {$unwind: "$destinations"},
    {$project: {firstName: 1, 'destinations.airport' : 1, 'destinations.nearbyAirfields.airfield' : 1}}
];
expectedRes = [
    {
        _id: 1,
        firstName: "Alice",
        destinations: {airport: "LHR", nearbyAirfields: [{airfield: "LHR"}]},
    },
    {
        _id: 2,
        firstName: "Alice",
        destinations: {airport: "ORD", nearbyAirfields: [{airfield: "ORD"}]},
    },
    {
        _id: 3,
        firstName: "Bob",
        destinations: {airport: "JFK", nearbyAirfields: [{airfield: "JFK"}]},
    }
];

assertGraphLookupExecution(
    pipeline, {comment: "sharded_to_sharded_lookup_view_to_sharded"}, expectedRes, [
        {
            // The 'travelers' collection is sharded, so the $graphLookup stage is executed in
            // parallel on every shard that contains the local collection.
            toplevelExec: [1, 1],
            // Each node executing the $graphLookup will, for every document that flows through the
            // stage, target the shard(s) that holds the relevant data for the sharded foreign view.
            recursiveMatchExec: [0, 3],
        },
        {
            collName: airportsColl.getName(),
            fromCollName: airfieldsColl.getName(),
            // When executing the subpipeline, the "nested" $lookup stage contained in the view
            // pipeline will stay on the merging half of the pipeline and execute on the merging
            // node, targeting shards to execute the nested subpipelines.
            toplevelExec: [0, 0],
            subpipelineExec: [1, 2]
        }
    ]);
mongosDB.airportsView.drop();

// Test that a targeted $graphLookup on a sharded collection can execute correctly on mongos.
st.shardColl(airportsColl, {airport: 1}, {airport: "LHR"}, {airport: "LHR"}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
pipeline = [
    {$group: {
        _id: "$_id",
        firstName: {$first: "$firstName"},
        nearestAirport: {$first: "$nearestAirport"}}
    },
    {$graphLookup: {
        from: "airports",
        startWith: "$nearestAirport",
        connectFromField: "connects",
        connectToField: "airport",
        maxDepth: 1,
        as: "destinations"
    }},
    {$project: {firstName: 1, 'destinations.airport': 1}}
];
expectedRes = [
    {
        _id: 1,
        firstName: "Alice",
        destinations: [{airport: "LHR"}, {airport: "ORD"}],
    },
    {
        _id: 2,
        firstName: "Alice",
        destinations: [{airport: "ORD"}, {airport: "PWM"}],
    },
    {
        _id: 3,
        firstName: "Bob",
        destinations: [{airport: "JFK"}, {airport: "LHR"}],
    }
];
assertGraphLookupExecution(
    pipeline,
    {comment: "sharded_to_sharded_on_mongos_targeted", allowDiskUse: false},
    expectedRes,
    [{
        // Because the $graphLookup is after a $group that requires merging, it is executed on
        // mongos.
        toplevelExec: [0, 0],
        mongosMerger: true,
        // For every document that flows through the $graphLookup stage, the mongos executing it
        // will target the shard that holds the relevant data for the sharded foreign collection.
        // This time, the query has only one cache for the $graphLookup, so four queries suffice.
        recursiveMatchExec: [1, 3]
    }]);

// Test that an untargeted $graphLookup on a sharded collection can execute correctly on mongos.
st.shardColl(airportsColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
assertGraphLookupExecution(
    pipeline,
    {comment: "sharded_to_sharded_on_mongos_untargeted", allowDiskUse: false},
    expectedRes,
    [{
        // Because the $graphLookup is after a $group that requires merging, it is executed on
        // mongos.
        toplevelExec: [0, 0],
        mongosMerger: true,
        // For every document that flows through the $graphLookup stage, the mongos executing it
        // will perform a scatter-gather query and open a cursor on every shard that contains the
        // foreign collection.
        recursiveMatchExec: [4, 4]
    }]);

// Test that a targeted $graphLookup on a sharded collection can execute correctly when mongos
// delegates to a merging shard.
st.shardColl(airportsColl, {airport: 1}, {airport: "LHR"}, {airport: "LHR"}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
assertGraphLookupExecution(
    pipeline,
    {comment: "sharded_to_sharded_on_merging_shard_targeted", allowDiskUse: true},
    expectedRes,
    [{
        // Because the $graphLookup is after a $group that requires merging, but 'allowDiskUse' is
        // true, the mongos delegates a merging shard to perform the $graphLookup execution.
        randomlyDelegatedMerger: true,
        // For every document that flows through the $graphLookup stage, the node executing it
        // will target the shard that holds the relevant data for the sharded foreign collection.
        recursiveMatchExec: [1, 3]
    }]);

// Test that an untargeted $graphLookup on a sharded collection can execute correctly when mongos
// delegates to a merging shard.
st.shardColl(airportsColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
assertGraphLookupExecution(
    pipeline,
    {comment: "sharded_to_sharded_on_merging_shard_untargeted", allowDiskUse: true},
    expectedRes,
    [{
        // Because the $graphLookup is after a $group that requires merging, but 'allowDiskUse' is
        // true, the mongos delegates a merging shard to perform the $graphLookup execution.
        randomlyDelegatedMerger: true,
        // For every document that flows through the $graphLookup stage, the node executing it
        // will perform a scatter-gather query and open a cursor on every shard that contains the
        //  foreign collection.
        recursiveMatchExec: [4, 4]
    }]);

// Test that multiple top-level $graphLookup stages are able to be run in parallel.
st.shardColl(airportsColl, {airport: 1}, {airport: "LHR"}, {airport: "LHR"}, mongosDB.getName());
st.shardColl(
    travelersColl, {firstName: 1}, {firstName: "Bob"}, {firstName: "Bob"}, mongosDB.getName());
st.shardColl(
    airfieldsColl, {airfield: 1}, {airfield: "LHR"}, {airfield: "LHR"}, mongosDB.getName());
pipeline = [
    {$graphLookup: {
        from: "airports",
        startWith: "$nearestAirport",
        connectFromField: "connects",
        connectToField: "airport",
        maxDepth: 0,
        as: "airports"
    }},
    {$graphLookup: {
        from: "airfields",
        startWith: "$airports.airport",
        connectFromField: "connects",
        connectToField: "airfield",
        maxDepth: 1,
        as: "airfields"
    }},
    {$project: {firstName: 1, 'airports.airport' : 1, 'airfields.airfield' : 1}}
];
expectedRes = [
    {
        _id: 1,
        firstName: "Alice",
        airports: [{"airport": "LHR"}],
        airfields: [{"airfield": "LHR"}, {"airfield": "MIA"}]
    },
    {
        _id: 2,
        firstName: "Alice",
        airports: [{"airport": "ORD"}],
        airfields: [{"airfield": "ORD"}, {"airfield": "MIA"}]
    },
    {
        _id: 3,
        firstName: "Bob",
        airports: [{"airport": "JFK"}],
        airfields: [{"airfield": "JFK"}, {"airfield": "MIA"}]
    }
];
assertGraphLookupExecution(pipeline, {comment: "multiple_graph_lookups"}, expectedRes, [
    {
        // The 'travelers' collection is sharded, so the $graphLookup stage is executed in parallel
        // on every shard that contains the local collection.
        toplevelExec: [1, 1],
        // Each node executing the $graphLookup will, for every document that flows through the
        // stage, target the shard(s) that holds the relevant data for the sharded foreign
        // collection.
        recursiveMatchExec: [1, 2],
    },
    {
        collName: travelersColl.getName(),
        fromCollName: airfieldsColl.getName(),
        // The second $graphLookup stage's expected execution behavior is similar to the first,
        // executing in parallel on every shard that contains the 'airfields' collection and, for
        // each node, targeting shards to execute the subpipeline.
        toplevelExec: [1, 1],
        recursiveMatchExec: [1, 4]
    }
]);

st.stop();
}());
