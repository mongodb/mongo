/**
 * Test that time-series collections work as expected with $unionWith.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   references_foreign_collection,
 *   requires_fcv_81,
 *   # Setting a server parameter is not allowed with a security token.
 *   not_allowed_with_signed_security_token,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getUnionWithStage, getNestedProperties, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/query/sbe_util.js";

const testDB = db.getSiblingDB(jsTestName());
const timeFieldName = "time";
const tagFieldName = "tag";

TimeseriesTest.run((insert) => {
    // TODO SERVER-93961: It may be possible to remove this when the shard role API is ready.
    // In slower builds, inserts may get stuck behind resharding operations. This fix lowers the
    // chance for that to occur.
    assert.commandWorked(testDB.adminCommand({setParameter: 1, maxRoundsWithoutProgressParameter: 10}));
    assert.commandWorked(testDB.dropDatabase());
    const collOptions = [null, {timeseries: {timeField: timeFieldName, metaField: tagFieldName}}];
    const numHosts = 10;
    const numDocs = 200;

    Random.setRandomSeed();
    const hosts = TimeseriesTest.generateHosts(numHosts);

    let testFunc = function (collAOption, collBOption) {
        // Prepare two time-series collections.
        const collA = testDB.getCollection("a");
        const collB = testDB.getCollection("b");
        collA.drop();
        collB.drop();
        assert.commandWorked(testDB.createCollection(collA.getName(), collAOption));
        assert.commandWorked(testDB.createCollection(collB.getName(), collBOption));
        let entryCountPerHost = new Array(numHosts).fill(0);
        let insertTimeseriesDoc = function (coll) {
            let host = TimeseriesTest.getRandomElem(hosts);
            assert.commandWorked(
                insert(coll, {
                    measurement: "cpu",
                    time: ISODate(),
                    tags: host.tags,
                }),
            );
            // Here we extract the hostId from "host.tags.hostname". It is expected that the
            // "host.tags.hostname" is in the form of 'host_<hostNum>'.
            return parseInt(host.tags.hostname.substring(5, host.tags.hostname.length));
        };
        for (let i = 0; i < numDocs; i++) {
            let hostId = insertTimeseriesDoc(collA);
            entryCountPerHost[hostId]++;
            hostId = insertTimeseriesDoc(collB);
            if (hostId % 2 == 0) {
                // Calculate the expected entry count per host. Later we will union collA entries
                // with the collB entries whose hostId is even.
                entryCountPerHost[hostId]++;
            }
        }

        const results = collA
            .aggregate([
                {
                    $unionWith: {
                        coll: collB.getName(),
                        pipeline: [
                            {
                                $match: {
                                    $expr: {
                                        $eq: [
                                            {
                                                $mod: [{$toInt: {$substr: ["$tags.hostname", 5, -1]}}, 2],
                                            },
                                            0,
                                        ],
                                    },
                                },
                            },
                        ],
                    },
                },
                {$group: {_id: "$tags.hostname", count: {$sum: 1}}},
                {$sort: {_id: 1}},
            ])
            .toArray();
        assert.eq(numHosts, results.length, results);
        for (let i = 0; i < numHosts; i++) {
            assert.eq({_id: "host_" + i, count: entryCountPerHost[i]}, results[i], results);
        }
    };

    // Exhaust the combinations of non-time-series and time-series collections for $unionWith
    // parameters.
    collOptions.forEach(function (collAOption) {
        collOptions.forEach(function (collBOption) {
            if (collAOption == null && collBOption == null) {
                // Normal $unionWith call, both inner and outer collections are non-time-series
                // collections.
                return;
            }
            testFunc(collAOption, collBOption);
        });
    });
});

const collA = testDB.getCollection("nested_a");
const collB = testDB.getCollection("nested_b");
const collC = testDB.getCollection("nested_c");

function setUpCollections() {
    collA.drop();
    collB.drop();
    collC.drop();
    assert.commandWorked(
        testDB.createCollection(collB.getName(), {timeseries: {timeField: timeFieldName, metaField: tagFieldName}}),
    );

    assert.commandWorked(collA.insertMany([{hostname: "host_0"}, {hostname: "host_1"}]));
    assert.commandWorked(
        collB.insertMany([
            {measurement: "cpu", time: ISODate(), tags: {hostname: "host_2"}},
            {measurement: "cpu", time: ISODate(), tags: {hostname: "host_3"}},
        ]),
    );
    assert.commandWorked(collC.insertMany([{hostname: "host_4"}, {hostname: "host_5"}]));
}

function getUnpackStage(explain) {
    const errMsg = "Expected to find an unpack stage in the $unionWith subpipeline. Explain: " + tojson(explain);
    const unionWithStage = getUnionWithStage(explain);
    if (checkSbeFullFeatureFlagEnabled(db)) {
        const unpack = getWinningPlanFromExplain(unionWithStage["$unionWith"]);
        assert.eq(unpack.stage, "UNPACK_TS_BUCKET", errMsg);
        return unpack;
    }
    const unpack = getNestedProperties(unionWithStage["$unionWith"]["pipeline"], "$_internalUnpackBucket");
    assert(unpack.length > 0, errMsg);
    return unpack[0];
}

(function nestedUnionWith() {
    setUpCollections();
    const pipeline = [
        {
            $unionWith: {
                coll: collB.getName(),
                pipeline: [
                    {
                        $unionWith: {
                            coll: collC.getName(),
                            pipeline: [{$match: {"hostname": "host_4"}}],
                        },
                    },
                    {$match: {$or: [{"tags.hostname": "host_2"}, {"hostname": "host_4"}]}},
                ],
            },
        },
        {$project: {[timeFieldName]: 0, _id: 0}},
    ];
    const results = collA.aggregate(pipeline).toArray();
    const expected = [
        {hostname: "host_0"},
        {hostname: "host_1"},
        {measurement: "cpu", tags: {hostname: "host_2"}},
        {hostname: "host_4"},
    ];
    assertArrayEq({actual: results, expected: expected});
})();

// Ensure unionWith optimizations work when the foreign collection is timeseries.
(function matchPushdown() {
    // The $match stage from the top-level pipeline is pushed into the subpipeline, even if the subpipeline is empty.
    setUpCollections();

    const pipeline = [
        {$unionWith: collB.getName()},
        {$match: {$or: [{"tags.hostname": "host_2"}, {"hostname": "host_1"}]}},
    ];

    // Don't use 'assertArrayEq' since we do not want to add a $project stage and it's difficult to assert on the _id field.
    const results = collA.aggregate(pipeline).toArray();
    assert.eq(results.length, 2);
    assert.eq(results[0].hostname, "host_1");
    assert.eq(results[1].measurement, "cpu");
    assert.eq(results[1].tags, {hostname: "host_2"});

    // Run explain to ensure the optimization took place.
    const unionWithStage = getUnionWithStage(collA.explain().aggregate(pipeline));
    const parsedQuery = getNestedProperties(unionWithStage["$unionWith"]["pipeline"], "parsedQuery");
    // If the optimization took place, the parsedQuery in the unionWith subpipeline will be non-empty and have the
    // $match predicate inside.
    assert(
        parsedQuery.length > 0 && Object.keys(parsedQuery[0]).length > 0,
        "Expected parsedQuery to contain an non-empty object. Explain: " + tojson(unionWithStage),
    );
})();

(function projectPushdown() {
    // The $project stage from the top-level pipeline is pushed into the subpipeline, even if the subpipeline is empty.
    setUpCollections();
    const pipeline = [{$unionWith: collB.getName()}, {$project: {hostname: 1, _id: 0, tags: 1}}];

    const results = collA.aggregate(pipeline).toArray();
    const expected = [
        {hostname: "host_0"},
        {hostname: "host_1"},
        {tags: {hostname: "host_2"}},
        {tags: {hostname: "host_3"}},
    ];
    assertArrayEq({actual: results, expected: expected});

    // Run explain to ensure the optimization took place.
    const explain = collA.explain().aggregate(pipeline);
    const unpackStage = getUnpackStage(explain);

    // If the optimization happened, then the unpack stage should have internalized the projection inside its "include" field.
    const includeArray = unpackStage.include;
    assertArrayEq({
        actual: includeArray,
        expected: ["tags", "hostname"],
        message: "Expected the unpack stage to internalize the projection. Explain: " + tojson(explain),
    });
})();
