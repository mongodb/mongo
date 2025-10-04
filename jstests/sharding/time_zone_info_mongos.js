// Test that mongoS accepts --timeZoneInfo <timezoneDBPath> as a command-line argument and that an
// aggregation pipeline with timezone expressions executes correctly on mongoS.
// Requires FCV 5.3 since $mergeCursors was added to explain output in 5.3.
// @tags: [
//   requires_fcv_53,
// ]
import {ShardingTest} from "jstests/libs/shardingtest.js";

const tzGoodInfoFat = "jstests/libs/config_files/good_timezone_info_fat";
const tzGoodInfoSlim = "jstests/libs/config_files/good_timezone_info_slim";
const tzBadInfo = "jstests/libs/config_files/bad_timezone_info";
const tzNoInfo = "jstests/libs/config_files/missing_directory";

function testWithGoodTimeZoneDir(tzGoodInfoDir) {
    const st = new ShardingTest({
        shards: 2,
        mongos: {s0: {timeZoneInfo: tzGoodInfoDir}},
        rs: {nodes: 1, timeZoneInfo: tzGoodInfoDir},
    });

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB[jsTestName()];

    assert.commandWorked(mongosDB.dropDatabase());

    // Confirm that the timeZoneInfo command-line argument has been set on mongoS.
    const mongosCfg = assert.commandWorked(mongosDB.adminCommand({getCmdLineOpts: 1}));
    assert.eq(mongosCfg.parsed.processManagement.timeZoneInfo, tzGoodInfoDir);

    // Test that a bad timezone file causes mongoS startup to fail.
    assert.throws(
        () => MongoRunner.runMongos({configdb: st.configRS.getURL(), timeZoneInfo: tzBadInfo}),
        [],
        "expected launching mongos with bad timezone rules to fail",
    );
    assert.neq(-1, rawMongoProgramOutput("Fatal assertion").search(/40475/));

    // Test that a non-existent timezone directory causes mongoS startup to fail.
    assert.throws(
        () => MongoRunner.runMongos({configdb: st.configRS.getURL(), timeZoneInfo: tzNoInfo}),
        [],
        "expected launching mongos with bad timezone rules to fail",
    );
    // Look for either old or new error message
    let output = rawMongoProgramOutput("(Error creating service context|Failed to create service context)");
    assert(output.includes("Error creating service context") || output.includes("Failed to create service context"));

    // Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.rs0.getURL()}));

    // Shard the test collection on _id.
    assert.commandWorked(mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey).
    assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

    // Move the [0, MaxKey) chunk to st.shard1.shardName.
    assert.commandWorked(
        mongosDB.adminCommand({moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}),
    );

    // Write a document containing a 'date' field to each chunk.
    assert.commandWorked(mongosColl.insert({_id: -1, date: ISODate("2017-11-13T12:00:00.000+0000")}));
    assert.commandWorked(mongosColl.insert({_id: 1, date: ISODate("2017-11-13T03:00:00.000+0600")}));
    assert.commandWorked(mongosColl.insert({_id: 2, date: ISODate("2020-10-20T19:49:47.634Z")}));
    // The test below failed on timelib-2018.01 on slim timezone format, as it's not supported.
    assert.commandWorked(mongosColl.insert({_id: 3, date: ISODate("2020-12-14T12:00:00Z")}));

    // Constructs a pipeline which splits the 'date' field into its constituent parts on mongoD,
    // reassembles the original date on mongoS, and verifies that the two match. All timezone
    // expressions in the pipeline use the passed 'tz' string or, if absent, default to "GMT".
    function buildTimeZonePipeline(tz) {
        // We use $const here so that the input pipeline matches the format of the explain output.
        const tzExpr = {$const: tz || "GMT"};
        return [
            {$addFields: {mongodParts: {$dateToParts: {date: "$date", timezone: tzExpr}}}},
            {$_internalSplitPipeline: {mergeType: st.getMergeType(mongosDB)}},
            {
                $addFields: {
                    mongosDate: {
                        $dateFromParts: {
                            year: "$mongodParts.year",
                            month: "$mongodParts.month",
                            day: "$mongodParts.day",
                            hour: "$mongodParts.hour",
                            minute: "$mongodParts.minute",
                            second: "$mongodParts.second",
                            millisecond: "$mongodParts.millisecond",
                            timezone: tzExpr,
                        },
                    },
                },
            },
            {$match: {$expr: {$eq: ["$date", "$mongosDate"]}}},
        ];
    }

    function testForTimezone(tz) {
        // Confirm that the pipe splits at '$_internalSplitPipeline' and that the merge runs on
        // mongoS.
        let timeZonePipeline = buildTimeZonePipeline(tz);
        const tzExplain = assert.commandWorked(mongosColl.explain().aggregate(timeZonePipeline));
        assert.eq(tzExplain.splitPipeline.shardsPart, [timeZonePipeline[0]]);
        // The first stage in the mergerPart will be $mergeCursors, so start comparing the pipelines
        // at the 1st index.
        assert.eq(tzExplain.splitPipeline.mergerPart.slice(1), timeZonePipeline.slice(1));
        assert.eq(tzExplain.mergeType, st.getMergeType(mongosDB));

        // Confirm that both documents are output by the pipeline, demonstrating that the date has
        // been correctly disassembled on mongoD and reassembled on mongoS.
        assert.eq(mongosColl.aggregate(timeZonePipeline).itcount(), 4);
    }

    testForTimezone("GMT");
    testForTimezone("America/New_York");

    // Confirm that aggregating with a timezone which is not present in 'tzGoodInfoDir' fails.
    let timeZonePipeline = buildTimeZonePipeline("Europe/Dublin");
    assert.eq(assert.throws(() => mongosColl.aggregate(timeZonePipeline)).code, 40485);

    st.stop();
}

testWithGoodTimeZoneDir(tzGoodInfoFat);
testWithGoodTimeZoneDir(tzGoodInfoSlim);
