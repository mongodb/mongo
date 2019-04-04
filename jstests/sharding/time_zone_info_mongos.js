// Test that merizoS accepts --timeZoneInfo <timezoneDBPath> as a command-line argument and that an
// aggregation pipeline with timezone expressions executes correctly on merizoS.
(function() {
    const tzGoodInfo = "jstests/libs/config_files/good_timezone_info";
    const tzBadInfo = "jstests/libs/config_files/bad_timezone_info";
    const tzNoInfo = "jstests/libs/config_files/missing_directory";

    const st = new ShardingTest({
        shards: 2,
        merizos: {s0: {timeZoneInfo: tzGoodInfo}},
        rs: {nodes: 1, timeZoneInfo: tzGoodInfo}
    });

    const merizosDB = st.s0.getDB(jsTestName());
    const merizosColl = merizosDB[jsTestName()];

    assert.commandWorked(merizosDB.dropDatabase());

    // Confirm that the timeZoneInfo command-line argument has been set on merizoS.
    const merizosCfg = assert.commandWorked(merizosDB.adminCommand({getCmdLineOpts: 1}));
    assert.eq(merizosCfg.parsed.processManagement.timeZoneInfo, tzGoodInfo);

    // Test that a bad timezone file causes merizoS startup to fail.
    let conn = MongoRunner.runMongos({configdb: st.configRS.getURL(), timeZoneInfo: tzBadInfo});
    assert.eq(conn, null, "expected launching merizos with bad timezone rules to fail");
    assert.neq(-1, rawMongoProgramOutput().indexOf("Fatal assertion 40475"));

    // Test that a non-existent timezone directory causes merizoS startup to fail.
    conn = MongoRunner.runMongos({configdb: st.configRS.getURL(), timeZoneInfo: tzNoInfo});
    assert.eq(conn, null, "expected launching merizos with bad timezone rules to fail");
    // Look for either old or new error message
    assert(rawMongoProgramOutput().indexOf("Failed to create service context") != -1 ||
           rawMongoProgramOutput().indexOf("Failed global initialization") != -1);

    // Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
    assert.commandWorked(merizosDB.adminCommand({enableSharding: merizosDB.getName()}));
    st.ensurePrimaryShard(merizosDB.getName(), st.rs0.getURL());

    // Shard the test collection on _id.
    assert.commandWorked(
        merizosDB.adminCommand({shardCollection: merizosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey).
    assert.commandWorked(
        merizosDB.adminCommand({split: merizosColl.getFullName(), middle: {_id: 0}}));

    // Move the [0, MaxKey) chunk to st.shard1.shardName.
    assert.commandWorked(merizosDB.adminCommand(
        {moveChunk: merizosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

    // Write a document containing a 'date' field to each chunk.
    assert.writeOK(merizosColl.insert({_id: -1, date: ISODate("2017-11-13T12:00:00.000+0000")}));
    assert.writeOK(merizosColl.insert({_id: 1, date: ISODate("2017-11-13T03:00:00.000+0600")}));

    // Constructs a pipeline which splits the 'date' field into its constituent parts on merizoD,
    // reassembles the original date on merizoS, and verifies that the two match. All timezone
    // expressions in the pipeline use the passed 'tz' string or, if absent, default to "GMT".
    function buildTimeZonePipeline(tz) {
        // We use $const here so that the input pipeline matches the format of the explain output.
        const tzExpr = {$const: (tz || "GMT")};
        return [
            {$addFields: {merizodParts: {$dateToParts: {date: "$date", timezone: tzExpr}}}},
            {$_internalSplitPipeline: {mergeType: "merizos"}},
            {
              $addFields: {
                  merizosDate: {
                      $dateFromParts: {
                          year: "$merizodParts.year",
                          month: "$merizodParts.month",
                          day: "$merizodParts.day",
                          hour: "$merizodParts.hour",
                          minute: "$merizodParts.minute",
                          second: "$merizodParts.second",
                          millisecond: "$merizodParts.millisecond",
                          timezone: tzExpr
                      }
                  }
              }
            },
            {$match: {$expr: {$eq: ["$date", "$merizosDate"]}}}
        ];
    }

    // Confirm that the pipe splits at '$_internalSplitPipeline' and that the merge runs on merizoS.
    let timeZonePipeline = buildTimeZonePipeline("GMT");
    const tzExplain = assert.commandWorked(merizosColl.explain().aggregate(timeZonePipeline));
    assert.eq(tzExplain.splitPipeline.shardsPart, [timeZonePipeline[0]]);
    assert.eq(tzExplain.splitPipeline.mergerPart, timeZonePipeline.slice(1));
    assert.eq(tzExplain.mergeType, "merizos");

    // Confirm that both documents are output by the pipeline, demonstrating that the date has been
    // correctly disassembled on merizoD and reassembled on merizoS.
    assert.eq(merizosColl.aggregate(timeZonePipeline).itcount(), 2);

    // Confirm that aggregating with a timezone which is not present in 'good_timezone_info' fails.
    timeZonePipeline = buildTimeZonePipeline("Europe/Dublin");
    assert.eq(assert.throws(() => merizosColl.aggregate(timeZonePipeline)).code, 40485);

    st.stop();
})();