/**
 * Tests that the 'rawData' flag is included in the query shape when present (for find, aggregate,
 * count, and distinct commands), and that:
 * - rawData: true produces a distinct query stats entry from rawData absent.
 * - rawData: false is normalized to absent and does NOT change the query shape.
 */
import {
    getLatestQueryStatsEntry,
    getQueryStatsAggCmd,
    getQueryStatsCountCmd,
    getQueryStatsDistinctCmd,
    getQueryStatsFindCmd,
    resetQueryStatsStore,
    withQueryStatsEnabled,
} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();

withQueryStatsEnabled(collName, (coll) => {
    /**
     * Runs the checks described in the test description above for a given command. 'makeCmd'
     * accepts a rawData value (true, false, or undefined for absent) and returns a command object.
     */
    function testRawDataShape(db, collName, makeCmd, getEntries, label) {
        resetQueryStatsStore(db.getMongo(), "1MB");
        assert.commandWorked(db.runCommand(makeCmd(undefined)));
        const entryAbsent = getLatestQueryStatsEntry(db.getMongo(), {collName});
        assert(entryAbsent, `${label}: expected entry without rawData`);
        assert(
            !entryAbsent.key.queryShape.hasOwnProperty("rawData"),
            `${label}: rawData should not appear in queryShape when absent. Shape: ${tojson(entryAbsent.key.queryShape)}`,
        );

        resetQueryStatsStore(db.getMongo(), "1MB");
        assert.commandWorked(db.runCommand(makeCmd(true)));
        const entryTrue = getLatestQueryStatsEntry(db.getMongo(), {collName});
        assert(entryTrue, `${label}: expected entry with rawData: true`);
        assert.eq(
            true,
            entryTrue.key.queryShape.rawData,
            `${label}: rawData should be true in queryShape. Shape: ${tojson(entryTrue.key.queryShape)}`,
        );

        resetQueryStatsStore(db.getMongo(), "1MB");
        assert.commandWorked(db.runCommand(makeCmd(false)));
        const entryFalse = getLatestQueryStatsEntry(db.getMongo(), {collName});
        assert(entryFalse, `${label}: expected entry with rawData: false`);
        assert(
            !entryFalse.key.queryShape.hasOwnProperty("rawData"),
            `${label}: rawData: false should not appear in queryShape. Shape: ${tojson(entryFalse.key.queryShape)}`,
        );
        assert.eq(
            entryAbsent.key.queryShapeHash,
            entryFalse.key.queryShapeHash,
            `${label}: rawData absent and false should have the same queryShapeHash`,
        );

        resetQueryStatsStore(db.getMongo(), "1MB");
        assert.commandWorked(db.runCommand(makeCmd(true)));
        assert.commandWorked(db.runCommand(makeCmd(true)));
        assert.commandWorked(db.runCommand(makeCmd(false)));
        assert.commandWorked(db.runCommand(makeCmd(undefined)));

        const allEntries = getEntries(db, {collName});
        assert.eq(
            2,
            allEntries.length,
            `${label}: rawData=true and rawData=absent/false should produce exactly 2 entries. Entries: ${tojson(allEntries)}`,
        );

        const trueEntry = allEntries.find((e) => e.key.queryShape.rawData === true);
        const absentEntry = allEntries.find((e) => !e.key.queryShape.hasOwnProperty("rawData"));
        assert(trueEntry, `${label}: expected entry with rawData=true`);
        assert(absentEntry, `${label}: expected entry with rawData absent`);

        assert.eq(
            2,
            trueEntry.metrics.execCount,
            `${label}: rawData=true entry should have execCount=2, got ${tojson(trueEntry.metrics)}`,
        );
        assert.eq(
            2,
            absentEntry.metrics.execCount,
            `${label}: rawData=absent/false entry should have execCount=2 (1 false + 1 absent), got ${tojson(absentEntry.metrics)}`,
        );
    }

    const db = coll.getDB();
    assert(coll.drop());
    assert.commandWorked(
        coll.insertMany([
            {_id: 1, v: 1, m: "a"},
            {_id: 2, v: 2, m: "a"},
            {_id: 3, v: 3, m: "b"},
        ]),
    );

    // find command.
    testRawDataShape(
        db,
        collName,
        (rawData) => {
            const cmd = {find: collName, filter: {v: {$gt: 0}}};
            if (rawData !== undefined) cmd.rawData = rawData;
            return cmd;
        },
        getQueryStatsFindCmd,
        "find",
    );

    // aggregate command.
    testRawDataShape(
        db,
        collName,
        (rawData) => {
            const cmd = {aggregate: collName, pipeline: [{$match: {v: {$gt: 0}}}], cursor: {}};
            if (rawData !== undefined) cmd.rawData = rawData;
            return cmd;
        },
        getQueryStatsAggCmd,
        "aggregate",
    );

    // count command.
    testRawDataShape(
        db,
        collName,
        (rawData) => {
            const cmd = {count: collName};
            if (rawData !== undefined) cmd.rawData = rawData;
            return cmd;
        },
        getQueryStatsCountCmd,
        "count",
    );

    // distinct command.
    testRawDataShape(
        db,
        collName,
        (rawData) => {
            const cmd = {distinct: collName, key: "m"};
            if (rawData !== undefined) cmd.rawData = rawData;
            return cmd;
        },
        getQueryStatsDistinctCmd,
        "distinct",
    );
});
