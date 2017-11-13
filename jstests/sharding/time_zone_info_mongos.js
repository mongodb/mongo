// Test that mongoS accepts --timeZoneInfo <timezoneDBPath> as a command-line argument and that an
// aggregation pipeline with timezone expressions executes correctly on mongoS.
(function() {
    const tzGoodInfo = "jstests/libs/config_files/good_timezone_info";
    const tzBadInfo = "jstests/libs/config_files/bad_timezone_info";
    const tzNoInfo = "jstests/libs/config_files/missing_directory";

    const st = new ShardingTest({
        shards: 2,
        mongos: {s0: {timeZoneInfo: tzGoodInfo}},
        rs: {nodes: 1, timeZoneInfo: tzGoodInfo}
    });

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB[jsTestName()];

    assert.commandWorked(mongosDB.dropDatabase());

    // Confirm that the timeZoneInfo command-line argument has been set on mongoS.
    const mongosCfg = assert.commandWorked(mongosDB.adminCommand({getCmdLineOpts: 1}));
    assert.eq(mongosCfg.parsed.processManagement.timeZoneInfo, tzGoodInfo);

    // Test that a bad timezone file causes mongoS startup to fail.
    let conn = MongoRunner.runMongos({configdb: st.configRS.getURL(), timeZoneInfo: tzBadInfo});
    assert.eq(conn, null, "expected launching mongos with bad timezone rules to fail");
    assert.neq(-1, rawMongoProgramOutput().indexOf("Fatal assertion 40475"));

    // Test that a non-existent timezone directory causes mongoS startup to fail.
    conn = MongoRunner.runMongos({configdb: st.configRS.getURL(), timeZoneInfo: tzNoInfo});
    assert.eq(conn, null, "expected launching mongos with bad timezone rules to fail");
    assert.neq(-1, rawMongoProgramOutput().indexOf("Failed global initialization"));

    // Enable sharding on the test DB and ensure its primary is shard0000.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Shard the test collection on _id.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey).
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

    // Move the [0, MaxKey) chunk to shard0001.
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

    // Write a document containing a 'date' field to each chunk.
    assert.writeOK(mongosColl.insert({_id: -1, date: ISODate("2017-11-13T12:00:00.000+0000")}));
    assert.writeOK(mongosColl.insert({_id: 1, date: ISODate("2017-11-13T03:00:00.000+0600")}));

    // Constructs a pipeline which splits the 'date' field into its constituent parts on mongoD,
    // reassembles the original date on mongoS, and verifies that the two match. All timezone
    // expressions in the pipeline use the passed 'tz' string or, if absent, default to "GMT".
    function buildTimeZonePipeline(tz) {
        // We use $const here so that the input pipeline matches the format of the explain output.
        const tzExpr = {$const: (tz || "GMT")};
        return [
            {$addFields: {mongodParts: {$dateToParts: {date: "$date", timezone: tzExpr}}}},
            {$_internalSplitPipeline: {mergeType: "mongos"}},
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
                          timezone: tzExpr
                      }
                  }
              }
            },
            {$match: {$expr: {$eq: ["$date", "$mongosDate"]}}}
        ];
    }

    // Confirm that the pipe splits at '$_internalSplitPipeline' and that the merge runs on mongoS.
    let timeZonePipeline = buildTimeZonePipeline("GMT");
    const tzExplain = assert.commandWorked(mongosColl.explain().aggregate(timeZonePipeline));
    assert.eq(tzExplain.splitPipeline.shardsPart, [timeZonePipeline[0]]);
    assert.eq(tzExplain.splitPipeline.mergerPart, timeZonePipeline.slice(1));
    assert.eq(tzExplain.mergeType, "mongos");

    // Confirm that both documents are output by the pipeline, demonstrating that the date has been
    // correctly disassembled on mongoD and reassembled on mongoS.
    assert.eq(mongosColl.aggregate(timeZonePipeline).itcount(), 2);

    // Confirm that aggregating with a timezone which is not present in 'good_timezone_info' fails.
    timeZonePipeline = buildTimeZonePipeline("Europe/Dublin");
    assert.eq(assert.throws(() => mongosColl.aggregate(timeZonePipeline)).code, 40485);

    st.stop();
})();