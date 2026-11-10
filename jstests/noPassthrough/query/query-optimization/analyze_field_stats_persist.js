/**
 * Tests the analyze command with mode: "fieldStats": parameter validation and the shape of the
 * field-stats documents it persists into system.stats.fieldStats.
 *
 * @tags: [
 *   featureFlagPersistentStats,
 * ]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {dropStatsColl} from "jstests/libs/query/persistent_samples_utils.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {areViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const kFieldStatsCollName = "system.stats.fieldStats";
const kExpectedSchemaVersion = 1;
const kExpectedPrecision = 14;

describe("analyze mode: fieldStats", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagPersistentStats: true,
                internalQueryEnablePersistentNDVStats: true,
            },
        });
        this.db = this.conn.getDB("test");
        this.coll = this.db[jsTestName()];
        this.fieldStats = this.db[kFieldStatsCollName];

        this.analyze = (options) =>
            this.db.runCommand(Object.assign({analyze: this.coll.getName()}, options));

        this.statsDocs = () => this.fieldStats.find().toArray();
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    beforeEach(function () {
        this.coll.drop();
        dropStatsColl(this.db, kFieldStatsCollName);
        // Three scalar/nullish values of "a" (7, 8 and missing); the object is excluded.
        // Two distinct values of "a.b" (1 and missing).
        assert.commandWorked(
            this.coll.insert([{a: 7}, {a: 7}, {a: 8}, {a: {b: 1}}, {other: true}]),
        );
    });

    it("requires a key", function () {
        assert.commandFailedWithCode(this.analyze({mode: "fieldStats"}), 13175800);
    });

    it("ignores sampling and histogram parameters", function () {
        assert.commandWorked(this.analyze({mode: "fieldStats", key: "a"}));
        const baseline = this.statsDocs()[0];
        for (const params of [
            {sampleRate: 0.5},
            {sampleSize: 100},
            {numberBuckets: 10},
            {samplingMethod: "random"},
            {numChunks: 5},
        ]) {
            assert.commandWorked(
                this.analyze(Object.assign({mode: "fieldStats", key: "a"}, params)),
            );
            const doc = this.statsDocs()[0];
            assert.eq(doc.ndv, baseline.ndv);
            assert.eq(doc.mcv, baseline.mcv);
        }
    });

    it("rejects invalid key paths", function () {
        assert.commandFailedWithCode(this.analyze({mode: "fieldStats", key: ""}), 6799703);
        assert.commandFailedWithCode(this.analyze({mode: "fieldStats", key: "a.0.b"}), 6799704);
        assert.commandFailed(this.analyze({mode: "fieldStats", key: "$a"}));
    });

    it("rejects timeseries collections and views", function () {
        const tsColl = jsTestName() + "_ts";
        assert.commandWorked(this.db.createCollection(tsColl, {timeseries: {timeField: "t"}}));
        assert.commandFailedWithCode(
            this.db.runCommand({analyze: tsColl, mode: "fieldStats", key: "a"}),
            areViewlessTimeseriesEnabled(this.db)
                ? ErrorCodes.CommandNotSupported
                : ErrorCodes.CommandNotSupportedOnView,
        );
        this.db[tsColl].drop();

        const view = jsTestName() + "_view";
        assert.commandWorked(this.db.createView(view, this.coll.getName(), []));
        assert.commandFailedWithCode(
            this.db.runCommand({analyze: view, mode: "fieldStats", key: "a"}),
            ErrorCodes.CommandNotSupportedOnView,
        );
        this.db[view].drop();
    });

    it("works on capped collections", function () {
        const cappedName = jsTestName() + "_capped";
        assert.commandWorked(
            this.db.createCollection(cappedName, {capped: true, size: 1024 * 1024}),
        );
        assert.commandWorked(this.db[cappedName].insert([{a: 1}, {a: 2}]));
        assert.commandWorked(
            this.db.runCommand({analyze: cappedName, mode: "fieldStats", key: "a"}),
        );

        const docs = this.statsDocs();
        assert.eq(docs.length, 1, "expected a stats doc for the capped collection", {docs});
        assert.eq(docs[0].ndv.sketches[0].ndv, NumberLong(2), "unexpected ndv", {docs});
        this.db[cappedName].drop();
    });

    it("rejects system collections", function () {
        assert.commandWorked(this.analyze({mode: "fieldStats", key: "a"}));
        assert.commandFailedWithCode(
            this.db.runCommand({analyze: kFieldStatsCollName, mode: "fieldStats", key: "a"}),
            13175804,
        );
    });

    it("fails on a nonexistent collection", function () {
        assert.commandFailedWithCode(
            this.db.runCommand({analyze: "does_not_exist", mode: "fieldStats", key: "a"}),
            13175802,
        );
    });

    it("persists a field-stats document with the expected schema", function () {
        assert.commandWorked(this.analyze({mode: "fieldStats", key: "a"}));

        const docs = this.statsDocs();
        assert.eq(docs.length, 1, "expected exactly one field-stats doc", {docs});
        const doc = docs[0];

        const collUuid = this.coll.getUUID();
        assert.eq(
            doc._id,
            `${kExpectedSchemaVersion}|${extractUUIDFromObject(collUuid)}|a`,
            "unexpected _id",
            {doc},
        );

        assert.eq(doc.schemaVersion, kExpectedSchemaVersion, "unexpected doc", {doc});
        assert.eq(bsonWoCompare(doc.collectionUuid, collUuid), 0, "unexpected doc", {doc});
        assert.eq(doc.sortedFieldPaths, ["a"], "unexpected doc", {doc});
        assert(doc.createdAt instanceof Date, "createdAt must be a date", {doc});

        const sketches = doc.ndv.sketches;
        assert.eq(sketches.length, 1, "expected one sketch per field", {doc});
        // Distinct scalar/nullish values of "a": 7, 8 and missing.
        assert.eq(sketches[0].ndv, NumberLong(3), "unexpected ndv", {doc});
        assert.eq(sketches[0].precision, kExpectedPrecision, "unexpected precision", {doc});
        assert(sketches[0].registers instanceof BinData, "registers must be BinData", {doc});

        assert.eq(doc.mcv.sketches.length, 1, "expected one MCV sketch per field", {doc});
        assert.sameMembers(doc.mcv.sketches[0].entries, [
            {value: 7, estFreq: NumberLong(2), err: NumberLong(0)},
            {value: 8, estFreq: NumberLong(1), err: NumberLong(0)},
        ]);
    });

    it("replaces the document when rerun and keys dotted paths separately", function () {
        assert.commandWorked(this.analyze({mode: "fieldStats", key: "a"}));
        const firstRun = this.statsDocs();
        assert.commandWorked(this.analyze({mode: "fieldStats", key: "a"}));
        assert.eq(this.statsDocs().length, 1, "rerun must replace, not duplicate");

        // A new distinct value must be reflected after a rerun, along with a fresh createdAt.
        assert.commandWorked(this.coll.insert({a: 9}));
        sleep(10);
        assert.commandWorked(this.analyze({mode: "fieldStats", key: "a"}));
        assert.eq(this.statsDocs()[0].ndv.sketches[0].ndv, NumberLong(4));
        assert.sameMembers(
            this.statsDocs()[0].mcv.sketches[0].entries.map((entry) => entry.value),
            [7, 8, 9],
        );
        assert.gt(this.statsDocs()[0].createdAt, firstRun[0].createdAt);

        // A different key gets its own document. Distinct values of "a.b": 1 and missing.
        assert.commandWorked(this.analyze({mode: "fieldStats", key: "a.b"}));
        const docs = this.statsDocs();
        assert.eq(docs.length, 2, "expected one doc per field", {docs});
        const dottedDoc = docs.find((doc) => doc.sortedFieldPaths[0] === "a.b");
        assert.eq(dottedDoc.ndv.sketches[0].ndv, NumberLong(2), "unexpected ndv", {dottedDoc});
    });

    it("persists a composite document with n+2 sketches for a tuple key", function () {
        // Four extra documents make all four sketches produce pairwise different counts, so a
        // duplicated or misordered sketch in the persisted array cannot go unnoticed. Tuples of
        // (a, other): (7, missing) x2, (8, missing), (missing, true),
        // (null, missing), (missing, missing), (7, null) and (null, true).
        assert.commandWorked(
            this.coll.insert([{a: null}, {x: 1}, {a: 7, other: null}, {a: null, other: true}]),
        );

        // Deliberately unsorted: the command canonicalizes the order.
        assert.commandWorked(this.analyze({mode: "fieldStats", key: ["other", "a"]}));

        const docs = this.statsDocs();
        assert.eq(docs.length, 1, "expected exactly one field-stats doc", {docs});
        const doc = docs[0];

        const collUuid = this.coll.getUUID();
        assert.eq(
            doc._id,
            `${kExpectedSchemaVersion}|${extractUUIDFromObject(collUuid)}|a|other`,
            "unexpected _id",
            {doc},
        );
        assert.eq(doc.sortedFieldPaths, ["a", "other"], "unexpected doc", {doc});

        assert.eq(doc.mcv.sketches.length, 2, "expected one MCV sketch per field", {doc});

        // n+2 sketches for n = 2 fields, positionally keyed off the sorted paths: the strict
        // tuple sketch first, then the variant folding "a" (which merges two tuple pairs), then
        // the variant folding "other" (which merges one), then all-match (which merges three).
        const sketches = doc.ndv.sketches;
        assert.eq(sketches.length, 4, "expected n+2 sketches", {doc});
        assert.eq(sketches[0].ndv, NumberLong(7), "unexpected strict ndv", {doc});
        assert.eq(sketches[1].ndv, NumberLong(5), "unexpected ndv folding 'a'", {doc});
        assert.eq(sketches[2].ndv, NumberLong(6), "unexpected ndv folding 'other'", {doc});
        assert.eq(sketches[3].ndv, NumberLong(4), "unexpected all-match ndv", {doc});
        for (const sketch of sketches) {
            assert.eq(sketch.precision, kExpectedPrecision, "unexpected precision", {doc});
            assert(sketch.registers instanceof BinData, "registers must be BinData", {doc});
        }
    });

    it("stores one document per tuple regardless of key order", function () {
        assert.commandWorked(this.analyze({mode: "fieldStats", key: ["a", "other"]}));
        assert.commandWorked(this.analyze({mode: "fieldStats", key: ["other", "a"]}));
        // The reversed order replaces the same document rather than creating a second one.
        assert.eq(this.statsDocs().length, 1, "expected a single doc per tuple");
    });

    it("rejects invalid composite keys", function () {
        assert.commandFailedWithCode(this.analyze({mode: "fieldStats", key: []}), 13176201);
        assert.commandFailedWithCode(
            this.analyze({mode: "fieldStats", key: ["a", "b", "c", "d"]}),
            13176201,
        );
        assert.commandFailedWithCode(this.analyze({mode: "fieldStats", key: ["a", "a"]}), 13176202);
        // Overlapping paths describe redundant NDV components.
        assert.commandFailedWithCode(
            this.analyze({mode: "fieldStats", key: ["a", "a.b"]}),
            13176203,
        );
        // Per-path validation applies to every tuple element.
        assert.commandFailedWithCode(this.analyze({mode: "fieldStats", key: ["a", ""]}), 6799703);
    });

    it("rejects the array key form outside fieldStats mode", function () {
        assert.commandFailedWithCode(
            this.analyze({mode: "histograms", key: ["a", "other"]}),
            13176200,
        );
        assert.commandFailedWithCode(this.analyze({mode: "sample", key: ["a", "other"]}), 13176200);
    });

    it("keys stats by collection UUID across drop and recreate", function () {
        assert.commandWorked(this.analyze({mode: "fieldStats", key: "a"}));
        const oldUuid = this.coll.getUUID();

        this.coll.drop();
        assert.commandWorked(this.coll.insert({a: 1}));
        assert.commandWorked(this.analyze({mode: "fieldStats", key: "a"}));

        // The recreated collection gets its own document; the old-UUID document stays behind
        // as an orphan (cleanup after collection drop is an accepted non-goal for now).
        const docs = this.statsDocs();
        assert.eq(docs.length, 2, "expected one doc per collection incarnation", {docs});
        const newDoc = docs.find((doc) => bsonWoCompare(doc.collectionUuid, oldUuid) !== 0);
        assert.eq(newDoc.ndv.sketches[0].ndv, NumberLong(1), "unexpected ndv", {docs});
    });

    it("keeps the previous stats doc when the collection has been emptied", function () {
        assert.commandWorked(this.analyze({mode: "fieldStats", key: "a"}));
        const before = this.statsDocs();
        assert.eq(before.length, 1);

        // An empty scan produces no new statistics, so the previous (now stale)
        // document stays. Stats invalidation is out of scope for now; the read path clamps
        // served estimates to the current collection cardinality, which covers this case.
        assert.commandWorked(this.coll.deleteMany({}));
        assert.commandWorked(this.analyze({mode: "fieldStats", key: "a"}));
        assert.eq(this.statsDocs(), before, "previous stats doc must remain");
    });

    it("excludes multikey documents from persisted NDV", function () {
        assert.commandWorked(this.coll.insert({a: [{b: 2}, {b: 3}]}));
        assert.commandWorked(this.analyze({mode: "fieldStats", key: "a.b"}));
        assert.eq(this.fieldStats.findOne().ndv.sketches[0].ndv, NumberLong(2));
    });

    it("rejects user $merge into the stats collection", function () {
        assert.commandFailedWithCode(
            this.db.runCommand({
                aggregate: this.coll.getName(),
                pipeline: [{$merge: {into: kFieldStatsCollName}}],
                cursor: {},
            }),
            31319,
        );
    });
});

describe("analyze mode: fieldStats gating", function () {
    it("fails without featureFlagPersistentStats", function () {
        // Explicitly disable to override all-feature-flags test variants.
        const conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagPersistentStats: false,
                internalQueryEnablePersistentNDVStats: true,
            },
        });
        try {
            const db = conn.getDB("test");
            assert.commandWorked(db[jsTestName()].insert({a: 1}));
            assert.commandFailedWithCode(
                db.runCommand({analyze: jsTestName(), mode: "fieldStats", key: "a"}),
                ErrorCodes.CommandNotSupported,
            );
        } finally {
            MongoRunner.stopMongod(conn);
        }
    });

    it("fails without the internalQueryEnablePersistentNDVStats knob", function () {
        const conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagPersistentStats: true,
                internalQueryEnablePersistentNDVStats: false,
            },
        });
        try {
            const db = conn.getDB("test");
            assert.commandWorked(db[jsTestName()].insert({a: 1}));
            assert.commandFailedWithCode(
                db.runCommand({analyze: jsTestName(), mode: "fieldStats", key: "a"}),
                ErrorCodes.CommandNotSupported,
            );
        } finally {
            MongoRunner.stopMongod(conn);
        }
    });

    // The fieldStats-mode-without-test-commands case lives in
    // analyze_field_stats_mode_test_commands_disabled.js: it needs a mongod started without test
    // commands, which is incompatible with suites that inject failpoint setParameters.
});
