/**
 * Tests that a primary-driven index build on a capped collection survives the documents around its
 * collection scan cursor rolling off.
 *
 * A two-phase build restarts its scan when this happens (see index_build_capped_position_lost.js).
 * A primary-driven build has already replicated its spills, so it cannot restart. Instead it
 * continues from the oldest surviving document, and keys for documents that rolled off are removed
 * when the side writes table is drained.
 *
 * @tags: [
 *   requires_capped,
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = jsTestName();
const primaryDB = primary.getDB(dbName);

const requiredFlags = ["PrimaryDrivenIndexBuilds", "ContainerWrites"];
for (const flag of requiredFlags) {
    if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, flag)) {
        jsTest.log.info("Skipping: required feature flag is disabled", {flag});
        rst.stopSet();
        quit();
    }
}

// Asserts that each index holds exactly one entry per document.
function assertIndexesMatchCollection(coll, indexNames) {
    const fromCollection = coll.find({}, {_id: 1}).hint({$natural: 1}).toArray();
    for (const indexName of indexNames) {
        const fromIndex = coll.find({}, {_id: 1}).hint(indexName).toArray();
        assert.sameMembers(
            fromCollection,
            fromIndex,
            "index entries differ from documents",
            undefined,
            {indexName},
        );
    }
    const res = assert.commandWorked(coll.validate());
    assert.eq(true, res.valid, "validate failed", {res});
}

// Starts a build of 'keyPatterns', waits for 'fp', calls 'rollOff' to roll off the documents around
// the scan cursor, then lets the build finish and checks the resulting indexes.
function buildAcrossRollOff(coll, keyPatterns, fp, rollOff, indexOptions = {}) {
    const indexNames = keyPatterns.map((keyPattern) => Object.keys(keyPattern)[0] + "_1");
    const awaitIndexBuild = IndexBuildTest.startIndexBuild(
        primary,
        coll.getFullName(),
        keyPatterns,
        indexOptions,
    );
    fp.wait();
    rollOff();
    fp.off();
    awaitIndexBuild();
    IndexBuildTest.assertIndexes(coll, 1 + indexNames.length, ["_id_", ...indexNames]);
    assertIndexesMatchCollection(coll, indexNames);
    assert(coll.drop());
}

describe("primary-driven index build on a capped collection that rolls over", function () {
    after(function () {
        rst.stopSet();
    });

    it("continues past documents that roll off during a collection scan yield", function () {
        // Yield after every document so the scan restores right after the failpoint releases.
        assert.commandWorked(
            primary.adminCommand({
                setParameter: 1,
                maxIndexBuildMemoryUsageMegabytes: 200,
                internalQueryExecYieldIterations: 1,
                internalQueryExecYieldPeriodMS: 10,
            }),
        );

        const coll = primaryDB.getCollection("scan_yield");
        assert.commandWorked(
            primaryDB.createCollection(coll.getName(), {capped: true, size: 1024 * 1024, max: 5}),
        );
        for (let i = 0; i < 5; i++) {
            assert.commandWorked(coll.insert({_id: i, a: i}));
        }

        const fp = configureFailPoint(
            primary,
            "hangIndexBuildDuringCollectionScanPhaseAfterInsertion",
            {
                fieldsToMatch: {a: 3},
            },
        );
        buildAcrossRollOff(coll, [{a: 1}], fp, () => {
            for (let i = 5; i < 10; i++) {
                assert.commandWorked(coll.insert({_id: i, a: i}));
            }
        });
    });

    it("continues past documents that roll off during a spill yield", function () {
        // A 1 MB sorter budget against 64 KB keys spills after a handful of documents, and a batch
        // size of one key makes every key of a spill a batch boundary. A large yield period keeps
        // the scan from yielding on its own, so the spill yield is where the cursor is restored.
        assert.commandWorked(
            primary.adminCommand({
                setParameter: 1,
                maxIndexBuildMemoryUsageMegabytes: 1,
                primaryDrivenIndexBuildSorterInsertionBatchSize: 1,
                internalQueryExecYieldIterations: 1000,
                internalQueryExecYieldPeriodMS: 60000,
            }),
        );

        const kMax = 40;
        const padding = "x".repeat(64 * 1024);
        const coll = primaryDB.getCollection("spill_yield");
        assert.commandWorked(
            primaryDB.createCollection(coll.getName(), {
                capped: true,
                size: 64 * 1024 * 1024,
                max: kMax,
            }),
        );
        for (let i = 0; i < kMax; i++) {
            assert.commandWorked(coll.insert({_id: i, a: `${i}-${padding}`}));
        }

        const fp = configureFailPoint(primary, "hangDuringIndexBuildSpillYield");
        buildAcrossRollOff(coll, [{a: 1}], fp, () => {
            for (let i = kMax; i < 2 * kMax; i++) {
                assert.commandWorked(coll.insert({_id: i, a: `${i}-${padding}`}));
            }
        });
    });

    it("continues past documents that roll off while two indexes spill", function () {
        // Same setup as the single-index spill case, but each index has its own sorter and spill
        // callbacks, so each one restores the shared scan cursor after its own spills.
        assert.commandWorked(
            primary.adminCommand({
                setParameter: 1,
                maxIndexBuildMemoryUsageMegabytes: 1,
                primaryDrivenIndexBuildSorterInsertionBatchSize: 1,
                internalQueryExecYieldIterations: 1000,
                internalQueryExecYieldPeriodMS: 60000,
            }),
        );

        const kMax = 40;
        const padding = "x".repeat(64 * 1024);
        const coll = primaryDB.getCollection("two_index_spill_yield");
        assert.commandWorked(
            primaryDB.createCollection(coll.getName(), {
                capped: true,
                size: 64 * 1024 * 1024,
                max: kMax,
            }),
        );
        for (let i = 0; i < kMax; i++) {
            assert.commandWorked(coll.insert({_id: i, a: `${i}-${padding}`, b: `${i}-${padding}`}));
        }

        const fp = configureFailPoint(primary, "hangDuringIndexBuildSpillYield");
        buildAcrossRollOff(coll, [{a: 1}, {b: 1}], fp, () => {
            for (let i = kMax; i < 2 * kMax; i++) {
                assert.commandWorked(
                    coll.insert({_id: i, a: `${i}-${padding}`, b: `${i}-${padding}`}),
                );
            }
        });
    });

    it("does not report a duplicate for a unique key reused after roll-off", function () {
        // The new documents reuse the keys of the ones they roll off, so the sorter sees each of
        // a: 0..3 twice. The drain removes the old keys before the uniqueness recheck at commit.
        assert.commandWorked(
            primary.adminCommand({
                setParameter: 1,
                maxIndexBuildMemoryUsageMegabytes: 200,
                internalQueryExecYieldIterations: 1,
                internalQueryExecYieldPeriodMS: 10,
            }),
        );

        const coll = primaryDB.getCollection("unique_reuse");
        assert.commandWorked(
            primaryDB.createCollection(coll.getName(), {capped: true, size: 1024 * 1024, max: 5}),
        );
        for (let i = 0; i < 5; i++) {
            assert.commandWorked(coll.insert({_id: i, a: i}));
        }

        const fp = configureFailPoint(
            primary,
            "hangIndexBuildDuringCollectionScanPhaseAfterInsertion",
            {
                fieldsToMatch: {_id: 3},
            },
        );
        buildAcrossRollOff(
            coll,
            [{a: 1}],
            fp,
            () => {
                for (let i = 0; i < 5; i++) {
                    assert.commandWorked(coll.insert({_id: 5 + i, a: i}));
                }
            },
            {unique: true},
        );
    });
});
