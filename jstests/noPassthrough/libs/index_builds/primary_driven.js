/**
 * Test library for resumable primary-driven index builds.
 *
 * Typical usage:
 *
 *   import {
 *       PrimaryDrivenResumableIndexBuildTest,
 *       PdibPhase,
 *   } from "jstests/noPassthrough/libs/index_builds/primary_driven.js";
 *
 *   const rst = PrimaryDrivenResumableIndexBuildTest.setUp();
 *
 *   PrimaryDrivenResumableIndexBuildTest.run(rst, {phase: PdibPhase.SCAN});
 *   PrimaryDrivenResumableIndexBuildTest.run(rst, {phase: PdibPhase.LOAD});
 *   PrimaryDrivenResumableIndexBuildTest.run(rst, {phase: PdibPhase.DRAIN});
 *
 *   PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
 *
 * One run() call exercises all three positions (beginning, middle, end) of the given phase,
 * dropping and re-creating the test collection between positions.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {
    findMetricsFiles,
    otelFileExportParams,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

/**
 * Which phase of the primary-driven index build to pause at before forcing a resume via step-up.
 *
 * @enum {string}
 */
export const PdibPhase = Object.freeze({
    SCAN: "scan",
    LOAD: "load",
    DRAIN: "drain",
});

/**
 * Where within a phase the build should be paused before forcing a resume.
 *
 * @enum {string}
 */
export const PdibPosition = Object.freeze({
    BEGINNING: "beginning",
    MIDDLE: "middle",
    END: "end",
});

/**
 * What to do with the old primary after each step-up.
 *
 * @enum {string}
 */
export const PdibFailoverMode = Object.freeze({
    NO_RESTART: "noRestart", // Leave it running; it becomes a secondary in-process.
    CLEAN_RESTART: "cleanRestart", // SIGTERM + restart.
    UNCLEAN_RESTART: "uncleanRestart", // SIGKILL + restart.
});

const ALL_POSITIONS = [PdibPosition.BEGINNING, PdibPosition.MIDDLE, PdibPosition.END];

// Each entry maps a phase to the fail point that pauses it and the log ID that fires when the fail
// point is hit. matchKey is the name of the array field in the fail-point data that the server
// compares against ("buildUUIDs" or "indexNames").
const PHASE_FAIL_POINTS = {
    [PdibPhase.SCAN]: {
        failPointName: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion",
        logId: 20386,
        matchKey: "buildUUIDs",
    },
    [PdibPhase.LOAD]: {
        failPointName: "hangIndexBuildDuringBulkLoadPhase",
        logId: 4924400,
        matchKey: "indexNames",
    },
    [PdibPhase.DRAIN]: {
        failPointName: "hangIndexBuildDuringDrainWritesPhase",
        logId: 4841800,
        matchKey: "indexNames",
    },
};

const DEFAULT_DOC_COUNT = 3_000;
const DEFAULT_INDEX_NAME = "pdib_resumable_idx";
const DEFAULT_SIDE_WRITES_COUNT = 8;
// We always build three indexes in a single createIndexes call so that PdibPosition can pick
// "first/middle/last" index for the per-phase fail points whose matchKey is `indexNames`.
const DEFAULT_INDEX_COUNT = 3;
// The document fields present in every seeded doc. Each is covered by exactly one index below:
// `a` by the unique index, `b` by the wildcard, `c` by the standard index.
const DEFAULT_INDEX_KEYS = ["a", "b", "c"];
// The three index specs built in one createIndexes call, in the fixed order that PdibPosition maps to
// (beginning → [0], middle → [1], end → [2]; see indexNameForPosition). Each entry is a full
// createIndexes spec minus `name` (the test assigns the name). We deliberately mix three index
// types so a single resume exercises all of them:
//   [0] unique   — {a:1}; `a` is a unique scalar key.
//   [1] wildcard — {"$**":1} projected to `b`; multikey via the two-element array `b`
//   [2] standard — {c:1}; multikey because `c` is array-valued
const DEFAULT_INDEX_SPECS = [
    {key: {a: 1}, unique: true},
    {key: {"$**": 1}, wildcardProjection: {b: 1}},
    {key: {c: 1}},
];
// Per-field key pad sizes (bytes), applied to fields a/b/c in defaultDocTemplate. Different sizes
// mean the sorters fill at different rates and spill at different points — richer coverage than
// identical sorters spilling in lock-step. With docCount=3000 and a 1 MB / index sorter budget the
// three indexes spill roughly 4 / 3 / 1 times. `b` is padded to 500 (half of the others' "natural"
// size) because it holds two array elements, so the wildcard-on-`b` still indexes ~1 KB of key data
// per document.
const DEFAULT_PAD_BYTES = [1500, 500, 500];
// `maxIndexBuildMemoryUsageMegabytes` is divided across all indexes in the build. With 3 indexes
// we want each sorter to behave like the original single-index test (1 MB / index → ~10 spills
// over 10 MB of keys), so the total is DEFAULT_INDEX_COUNT MB.
const DEFAULT_MAX_INDEX_BUILD_MEM_MB = DEFAULT_INDEX_COUNT;
// 100 gives ~30 checkpoints per index over the default 3k keys — plenty for "multiple writes
// during load".
const DEFAULT_LOAD_RESUME_STATE_WRITE_INTERVAL_KEYS = 100;

const RESUME_SUCCEEDED_METRIC = "index_builds.resume.succeeded";
const RESUME_FAILED_METRIC = "index_builds.resume.failed";

// Server-side phase strings emitted as the `phase` attribute on index_builds.resume.succeeded.
const RESUME_PHASE_INITIALIZED = "initialized";
const RESUME_PHASE_SCAN = "collection scan";
const RESUME_PHASE_LOAD = "bulk load";
const RESUME_PHASE_DRAIN = "drain writes";

// Which `phase` attributes are plausible for each (PdibPhase, PdibPosition). At "beginning" of a
// phase the most recent checkpoint may still be from the prior phase, so we accept either.
// "middle"/"end" should always reflect the current phase.
const EXPECTED_RESUME_PHASES = {
    [PdibPhase.SCAN]: {
        [PdibPosition.BEGINNING]: [RESUME_PHASE_INITIALIZED, RESUME_PHASE_SCAN],
        [PdibPosition.MIDDLE]: [RESUME_PHASE_SCAN],
        [PdibPosition.END]: [RESUME_PHASE_SCAN],
    },
    [PdibPhase.LOAD]: {
        [PdibPosition.BEGINNING]: [RESUME_PHASE_SCAN, RESUME_PHASE_LOAD],
        [PdibPosition.MIDDLE]: [RESUME_PHASE_LOAD],
        [PdibPosition.END]: [RESUME_PHASE_LOAD],
    },
    [PdibPhase.DRAIN]: {
        [PdibPosition.BEGINNING]: [RESUME_PHASE_DRAIN],
        [PdibPosition.MIDDLE]: [RESUME_PHASE_DRAIN],
        [PdibPosition.END]: [RESUME_PHASE_DRAIN],
    },
};

function defaultDocTemplate(i) {
    // Each field gets a string whose length is set by `DEFAULT_PAD_BYTES`. Varying sizes mean the
    // sorters fill at different rates and spill at different points, exercising a more realistic mix
    // of per-index resume-state checkpoint boundaries than lock-step sorters would. The field shapes
    // also drive the index-type coverage in DEFAULT_INDEX_SPECS:
    //   a — scalar, and unique (the zero-padded prefix makes it distinct per document).
    //   b — two-element array. `b` is indexed only by the {"$**":1} wildcard (the middle-position
    //       index), so making it multi-valued gives genuine multi-element multikey coverage without
    //       perturbing the end-position index's per-document key count.
    //   c — single-element array. Enough to mark the {c:1} index multikey while still emitting
    //       exactly one key per document, so the LOAD/DRAIN end-position key-count math (which
    //       assumes ~1 key per doc for index[2]) stays intact.
    const prefix = String(i).padStart(8, "0");
    const pad = (n, ch = "x") => prefix + ch.repeat(n);
    return {
        _id: i,
        a: pad(DEFAULT_PAD_BYTES[0]),
        b: [pad(DEFAULT_PAD_BYTES[1], "x"), pad(DEFAULT_PAD_BYTES[1], "y")],
        c: [pad(DEFAULT_PAD_BYTES[2])],
    };
}

function bulkInsert(coll, count, template, batchSize = 1000) {
    let i = 0;
    while (i < count) {
        const bulk = coll.initializeUnorderedBulkOp();
        const end = Math.min(i + batchSize, count);
        for (let j = i; j < end; j++) {
            bulk.insert(template(j));
        }
        assert.commandWorked(bulk.execute());
        i = end;
    }
}

// Position N-1 (instead of N) so the fail point still has work left when it fires; floor(N/2) for
// the midpoint. For "beginning", iteration 0 fires before any work happens.
function iterationFor(position, total) {
    switch (position) {
        case PdibPosition.BEGINNING:
            return 0;
        case PdibPosition.MIDDLE:
            return Math.floor(total / 2);
        case PdibPosition.END:
            return Math.max(0, total - 1);
        default:
            throw new Error("Unknown PdibPosition: " + position);
    }
}

function totalIterationsFor(phase, docCount, sideWritesCount) {
    switch (phase) {
        case PdibPhase.SCAN:
        case PdibPhase.LOAD:
            return docCount;
        case PdibPhase.DRAIN:
            return sideWritesCount;
        default:
            throw new Error("Unknown PdibPhase: " + phase);
    }
}

// PdibPosition picks "which index" for fail points whose matchKey is `indexNames` (LOAD and
// DRAIN): BEGINNING → first, MIDDLE → middle (second), END → last (third). This must be called
// with an array of length DEFAULT_INDEX_COUNT.
function indexNameForPosition(indexNames, position) {
    assert.eq(indexNames.length, DEFAULT_INDEX_COUNT, "indexNameForPosition expects 3 index names");
    switch (position) {
        case PdibPosition.BEGINNING:
            return indexNames[0];
        case PdibPosition.MIDDLE:
            return indexNames[1];
        case PdibPosition.END:
            return indexNames[2];
        default:
            throw new Error("Unknown PdibPosition: " + position);
    }
}

export const PrimaryDrivenResumableIndexBuildTest = class {
    /**
     * Build and start a ReplSetTest with the OTel file exporter wired up so run() can verify
     * index_builds.resume.* metrics. The metricsDir is stashed on the rst as `_pdibMetricsDir`;
     * callers don't need to know where it lives. Always pair with tearDown(rst).
     *
     * @param {Object} [opts]
     * @param {string} [opts.testName=jsTestName()] - Stem for the OTel metrics directory.
     * @param {number} [opts.nodes=2] - Number of replica-set members. Use at least 3 for the
     *     CLEAN_RESTART / UNCLEAN_RESTART failover modes, where the primary is taken down before
     *     the secondary is stepped up: with only 2 nodes the surviving secondary can't form a
     *     majority, so the election would fail.
     * @returns {ReplSetTest} A started replica set with `_pdibMetricsDir` attached for use by
     *     `run()` / `runMultiPhase()`.
     */
    static setUp({testName, nodes = 2} = {}) {
        const name = testName || jsTestName();
        const {metricsDir, otelParams} = otelFileExportParams(name);

        // When graceful stepdown is not supported, fail over by checkpointing on the primary,
        // waiting for the secondary to install that checkpoint, then killing the primary. 2 nodes
        // is sufficient for this case.
        if (TestData.doesNotSupportGracefulStepdown) {
            nodes = 2;
        }

        // Ensure every node is electable (priority 1) so the failover tests can always step up any
        // secondary.
        const nodeConfigs = Array.from({length: nodes}, () => ({rsConfig: {priority: 1}}));

        // Slow down OTel flushes so the per-node JSONL stays under the shell cat() 16 MiB cap on
        // long-running variants (e.g. TSAN); _readResumeMetrics only needs the latest snapshot.
        const rstOptions = {
            nodes: nodeConfigs,
            nodeOptions: {setParameter: {...otelParams, openTelemetryExportIntervalMillis: 5000}},
        };

        const rst = new ReplSetTest(rstOptions);
        rst.startSet();
        rst.initiate();
        rst._pdibMetricsDir = metricsDir;
        return rst;
    }

    /**
     * Stops the replica set started by `setUp()`.
     *
     * @param {ReplSetTest} rst
     * @returns {void}
     */
    static tearDown(rst) {
        rst.stopSet();
    }

    /**
     * Run resumable PDIB tests for one phase. Iterates over `positions` (default: all three),
     * exercising one resume per position with a freshly-seeded collection.
     *
     * @param {ReplSetTest} rst - Must have been constructed via `setUp()`.
     * @param {Object} options
     * @param {string} options.phase - One of `PdibPhase`.
     * @param {string[]} [options.positions=ALL_POSITIONS] - Subset of `PdibPosition` values to
     *     exercise.
     * @param {string} [options.failoverMode=PdibFailoverMode.NO_RESTART] - What to do with the
     *     old primary after each step-up. See `PdibFailoverMode`.
     * @param {string} [options.dbName=jsTestName()]
     * @param {string} [options.collName="coll"]
     * @param {Object[]} [options.indexSpecs=DEFAULT_INDEX_SPECS] - The three index specs to build in
     *     a single `createIndexes` call, in beginning/middle/end order. Each entry is a full
     *     createIndexes index spec *without* `name` (the test assigns the name), e.g.
     *     `{key: {a: 1}, unique: true}` or `{key: {"$**": 1}}`. Must have length 3.
     * @param {Function} [options.docTemplate=defaultDocTemplate] - `(i: number) => Object`
     *     producing the i-th document.
     * @param {number} [options.docCount=DEFAULT_DOC_COUNT]
     * @param {Object[]} [options.sideWrites] - Defaults to a small fixed set of side writes that
     *     don't collide with the seeded docs.
     * @param {Object[]} [options.postIndexBuildInserts=[]] - Optional inserts to run after each
     *     successful resume to verify the index continues to work.
     * @param {number} [options.maxIndexBuildMemoryUsageMegabytes=DEFAULT_MAX_INDEX_BUILD_MEM_MB]
     *     - Server-parameter override.
     * @param {number} [options.loadResumeStateWriteIntervalKeys=DEFAULT_LOAD_RESUME_STATE_WRITE_INTERVAL_KEYS]
     *     - Server-parameter override.
     * @returns {void}
     */
    static run(rst, options) {
        const phase = options.phase;
        assert(phase, "options.phase is required");
        const positions = options.positions || ALL_POSITIONS;
        const failoverMode = options.failoverMode || PdibFailoverMode.NO_RESTART;
        assert(
            Object.values(PdibFailoverMode).includes(failoverMode),
            `options.failoverMode=${failoverMode} is not a valid PdibFailoverMode value`,
        );

        const dbName = options.dbName || jsTestName();
        const collName = options.collName || "coll";
        const indexSpecs = options.indexSpecs || DEFAULT_INDEX_SPECS;
        assert.eq(
            indexSpecs.length,
            DEFAULT_INDEX_COUNT,
            `options.indexSpecs must have length ${DEFAULT_INDEX_COUNT}`,
        );
        const docTemplate = options.docTemplate || defaultDocTemplate;
        const docCount = options.docCount || DEFAULT_DOC_COUNT;
        const sideWrites =
            options.sideWrites || PrimaryDrivenResumableIndexBuildTest._defaultSideWrites(docCount);
        const postIndexBuildInserts = options.postIndexBuildInserts || [];
        const maxMb = options.maxIndexBuildMemoryUsageMegabytes || DEFAULT_MAX_INDEX_BUILD_MEM_MB;
        const loadIntervalKeys =
            options.loadResumeStateWriteIntervalKeys ||
            DEFAULT_LOAD_RESUME_STATE_WRITE_INTERVAL_KEYS;

        if (phase === PdibPhase.DRAIN) {
            assert.gt(sideWrites.length, 0, "drain writes phase requires at least one side write");
        }

        if (
            !PrimaryDrivenResumableIndexBuildTest._featureFlagsEnabled(
                rst.getPrimary().getDB(dbName),
            )
        ) {
            jsTest.log.info(
                "PrimaryDrivenResumableIndexBuildTest: skipping because " +
                    "featureFlagContainerWrites or " +
                    "featureFlagPrimaryDrivenIndexBuilds or " +
                    "featureFlagResumablePrimaryDrivenIndexBuilds is disabled",
            );
            return;
        }
        assert.gte(
            rst.nodes.length,
            2,
            "PrimaryDrivenResumableIndexBuildTest requires a replica set with >= 2 nodes",
        );
        assert(
            rst._pdibMetricsDir,
            "rst must be constructed via PrimaryDrivenResumableIndexBuildTest.setUp() so the OTel " +
                "exporter is configured for index_builds.resume.* metric verification",
        );

        PrimaryDrivenResumableIndexBuildTest._setIndexBuildSettings(rst, {
            maxIndexBuildMemoryUsageMegabytes: maxMb,
            primaryDrivenIndexBuildLoadResumeStateWriteIntervalKeys: loadIntervalKeys,
        });

        for (const position of positions) {
            jsTest.log.info(
                `PrimaryDrivenResumableIndexBuildTest: phase=${phase} position=${position}`,
            );
            PrimaryDrivenResumableIndexBuildTest._runOne(rst, {
                phase,
                position,
                failoverMode,
                dbName,
                collName,
                indexSpecs,
                docTemplate,
                docCount,
                sideWrites,
                postIndexBuildInserts,
            });
        }
    }

    /**
     * Runs a single resumable primary-driven index build that is paused once in each phase
     * (scan, then load, then drain) and resumed across three step-ups. The position within each
     * phase is configurable via `options.positions`; phases not listed default to
     * `PdibPosition.MIDDLE`. Verifies that each resume increments `index_builds.resume.succeeded`
     * with the expected `phase` attribute and that the final index is consistent across nodes.
     *
     * @param {ReplSetTest} rst - Must have been constructed via `setUp()`.
     * @param {Object} [options]
     * @param {Object<string, string>} [options.positions={}] - Map from `PdibPhase` value to
     *     `PdibPosition` value selecting where in each phase the build pauses. Omitted phases
     *     default to `PdibPosition.MIDDLE`.
     * @param {string} [options.failoverMode=PdibFailoverMode.NO_RESTART] - What to do with the
     *     old primary after each step-up. See `PdibFailoverMode`.
     * @param {string} [options.dbName=jsTestName()]
     * @param {string} [options.collName="coll"]
     * @param {Object[]} [options.indexSpecs=DEFAULT_INDEX_SPECS] - The three index specs to build in
     *     a single `createIndexes` call, in beginning/middle/end order. Each entry is a full
     *     createIndexes index spec *without* `name` (the test assigns the name), e.g.
     *     `{key: {a: 1}, unique: true}` or `{key: {"$**": 1}}`. Must have length 3.
     * @param {Function} [options.docTemplate=defaultDocTemplate]
     * @param {number} [options.docCount=DEFAULT_DOC_COUNT]
     * @param {Object[]} [options.sideWrites] - Must contain at least one side write so that the
     *     DRAIN phase has work to do.
     * @param {Object[]} [options.postIndexBuildInserts=[]]
     * @param {number} [options.maxIndexBuildMemoryUsageMegabytes=DEFAULT_MAX_INDEX_BUILD_MEM_MB]
     * @param {number} [options.loadResumeStateWriteIntervalKeys=DEFAULT_LOAD_RESUME_STATE_WRITE_INTERVAL_KEYS]
     * @returns {void}
     */
    static runMultiPhase(rst, options = {}) {
        const dbName = options.dbName || jsTestName();
        const collName = options.collName || "coll";
        const indexSpecs = options.indexSpecs || DEFAULT_INDEX_SPECS;
        assert.eq(
            indexSpecs.length,
            DEFAULT_INDEX_COUNT,
            `options.indexSpecs must have length ${DEFAULT_INDEX_COUNT}`,
        );
        const docTemplate = options.docTemplate || defaultDocTemplate;
        const docCount = options.docCount || DEFAULT_DOC_COUNT;
        const sideWrites =
            options.sideWrites || PrimaryDrivenResumableIndexBuildTest._defaultSideWrites(docCount);
        const postIndexBuildInserts = options.postIndexBuildInserts || [];
        const maxMb = options.maxIndexBuildMemoryUsageMegabytes || DEFAULT_MAX_INDEX_BUILD_MEM_MB;
        const loadIntervalKeys =
            options.loadResumeStateWriteIntervalKeys ||
            DEFAULT_LOAD_RESUME_STATE_WRITE_INTERVAL_KEYS;

        const positions = options.positions || {};
        const validPositions = Object.values(PdibPosition);
        for (const [phase, position] of Object.entries(positions)) {
            assert(
                validPositions.includes(position),
                `options.positions[${phase}]=${position} is not a valid PdibPosition value`,
            );
        }
        const posFor = (phase) => positions[phase] || PdibPosition.MIDDLE;

        const failoverMode = options.failoverMode || PdibFailoverMode.NO_RESTART;
        assert(
            Object.values(PdibFailoverMode).includes(failoverMode),
            `options.failoverMode=${failoverMode} is not a valid PdibFailoverMode value`,
        );

        // Side writes are required because the `hangIndexBuildDuringDrainWritesPhase` fail point is
        // skipped when there are no writes to drain.
        assert.gt(
            sideWrites.length,
            0,
            "runMultiPhase requires at least one side write so that the DRAIN phase has work",
        );

        if (
            !PrimaryDrivenResumableIndexBuildTest._featureFlagsEnabled(
                rst.getPrimary().getDB(dbName),
            )
        ) {
            jsTest.log.info(
                "PrimaryDrivenResumableIndexBuildTest: skipping runMultiPhase because " +
                    "featureFlagContainerWrites or " +
                    "featureFlagPrimaryDrivenIndexBuilds or " +
                    "featureFlagResumablePrimaryDrivenIndexBuilds is disabled",
            );
            return;
        }
        assert.gte(
            rst.nodes.length,
            2,
            "PrimaryDrivenResumableIndexBuildTest requires a replica set with >= 2 nodes",
        );
        assert(
            rst._pdibMetricsDir,
            "rst must be constructed via PrimaryDrivenResumableIndexBuildTest.setUp() so the OTel " +
                "exporter is configured for index_builds.resume.* metric verification",
        );

        PrimaryDrivenResumableIndexBuildTest._setIndexBuildSettings(rst, {
            maxIndexBuildMemoryUsageMegabytes: maxMb,
            primaryDrivenIndexBuildLoadResumeStateWriteIntervalKeys: loadIntervalKeys,
        });

        // Seed the collection while the initial primary is in charge.
        let primary = rst.getPrimary();
        const coll = primary.getDB(dbName).getCollection(collName);
        coll.drop();
        bulkInsert(coll, docCount, docTemplate);
        rst.awaitReplication();

        const indexNames = Array.from(
            {length: DEFAULT_INDEX_COUNT},
            (_, i) => `${DEFAULT_INDEX_NAME}_multi_phase_${i}`,
        );

        // Start the build on the initial primary; it hangs at hangBeforeBuildingIndex.
        const {awaitCreateIndexes, buildUUID, hangBeforeBuildingIndexFp} =
            PrimaryDrivenResumableIndexBuildTest._startBuild(
                rst,
                dbName,
                coll,
                indexSpecs,
                indexNames,
                sideWrites,
            );

        // Configure the SCAN fail point on the initial primary at the configured position, then
        // release hangBeforeBuildingIndex.
        let currentFp = PrimaryDrivenResumableIndexBuildTest._configurePhaseFailPoint(
            primary,
            PdibPhase.SCAN,
            posFor(PdibPhase.SCAN),
            buildUUID,
            indexNames,
            docCount,
            sideWrites.length,
        );
        hangBeforeBuildingIndexFp.off();
        PrimaryDrivenResumableIndexBuildTest._waitForPause(
            primary,
            PHASE_FAIL_POINTS[PdibPhase.SCAN],
            buildUUID,
            indexNameForPosition(indexNames, posFor(PdibPhase.SCAN)),
        );

        // Step-up sequence. After each step-up, the new primary resumes the build from the
        // previous phase's checkpoint and (for the first two iterations) pauses again at the
        // next phase's configured position. The final iteration has no nextPhase: the new
        // primary resumes from DRAIN and runs to completion.
        const stepUps = [
            {previousPhase: PdibPhase.SCAN, nextPhase: PdibPhase.LOAD},
            {previousPhase: PdibPhase.LOAD, nextPhase: PdibPhase.DRAIN},
            {previousPhase: PdibPhase.DRAIN, nextPhase: null},
        ];

        let shellDrained = false;

        for (const {previousPhase, nextPhase} of stepUps) {
            // Configure the next-phase fail point on the node about to be stepped up BEFORE the
            // step-up, so the resumed build hangs there as soon as it reaches that phase. The
            // last iteration has no nextPhase (we want completion, not another pause).
            const nextPrimaryNode = rst.getSecondary();
            let nextFp = null;
            if (nextPhase !== null) {
                nextFp = PrimaryDrivenResumableIndexBuildTest._configurePhaseFailPoint(
                    nextPrimaryNode,
                    nextPhase,
                    posFor(nextPhase),
                    buildUUID,
                    indexNames,
                    docCount,
                    sideWrites.length,
                );
            }

            // Snapshot metrics so we can assert that this resume incremented
            // succeeded[previousPhase] exactly once.
            const beforeMetrics = PrimaryDrivenResumableIndexBuildTest._readResumeMetrics(
                rst._pdibMetricsDir,
            );

            const oldPrimary = rst.getPrimary();
            jsTest.log.info(
                `PrimaryDrivenResumableIndexBuildTest: failover mode=${failoverMode}, stepping up ` +
                    `${nextPrimaryNode.host} to resume from ${previousPhase}+${posFor(previousPhase)}` +
                    (nextPhase === null
                        ? " and complete"
                        : ` and pause at ${nextPhase}+${posFor(nextPhase)}`),
            );

            // For restart modes, wait for the index build resume state to replicate to secondaries
            // before shutting down the primary. Container writes (including resume state) replicate
            // asynchronously, so without this the stepped-up node could win the election without
            // the resume state, preventing it from resuming the build.
            if (
                failoverMode === PdibFailoverMode.UNCLEAN_RESTART ||
                failoverMode === PdibFailoverMode.CLEAN_RESTART
            ) {
                rst.awaitReplication();
            }

            // Step up first so the state change interrupts the old primary's build thread at its
            // fail point. If we released `currentFp` first, the build would simply continue past
            // the iteration on the old primary and could complete before we ever stepped up.
            const newPrimary = PrimaryDrivenResumableIndexBuildTest._failover(
                rst,
                oldPrimary,
                nextPrimaryNode,
                failoverMode,
            );

            // Release the current fail point (cleanup; the build thread on the old primary has
            // already exited due to the state change). For restart modes the old mongod was
            // recycled, so its in-memory fail point is already gone and the handle's connection
            // is stale — skip. When graceful stepdown isn't supported the failover always kills the
            // old primary (see `_failOverWithCheckpointInstall`), so skip in that case too.
            if (
                failoverMode === PdibFailoverMode.NO_RESTART &&
                !TestData.doesNotSupportGracefulStepdown
            ) {
                currentFp.off();
            }

            if (!shellDrained) {
                // The parallel shell that called createIndexes against the very first primary
                // returns InterruptedDueToReplStateChange after the first step-down. Drain it
                // once so it doesn't outlive the test.
                awaitCreateIndexes({checkExitSuccess: false});
                shellDrained = true;
            }

            if (nextPhase !== null) {
                PrimaryDrivenResumableIndexBuildTest._waitForPause(
                    newPrimary,
                    PHASE_FAIL_POINTS[nextPhase],
                    buildUUID,
                    indexNameForPosition(indexNames, posFor(nextPhase)),
                );
            } else {
                jsTest.log.info(
                    `PrimaryDrivenResumableIndexBuildTest: waiting for build ${buildUUID} to ` +
                        `complete on ${newPrimary.host}`,
                );
                PrimaryDrivenResumableIndexBuildTest._waitForBuildOutcome(
                    newPrimary,
                    dbName,
                    collName,
                    indexNames,
                    buildUUID,
                );
            }

            PrimaryDrivenResumableIndexBuildTest._verifyResumeMetric(
                rst._pdibMetricsDir,
                beforeMetrics,
                EXPECTED_RESUME_PHASES[previousPhase][posFor(previousPhase)],
            );

            currentFp = nextFp;
        }

        rst.awaitReplication();
        PrimaryDrivenResumableIndexBuildTest._verifyIndexAcrossNodes(
            rst,
            dbName,
            collName,
            indexNames,
        );

        if (postIndexBuildInserts.length > 0) {
            const finalPrimary = rst.getPrimary();
            assert.commandWorked(
                finalPrimary.getDB(dbName).getCollection(collName).insert(postIndexBuildInserts),
            );
            rst.awaitReplication();
            PrimaryDrivenResumableIndexBuildTest._verifyIndexAcrossNodes(
                rst,
                dbName,
                collName,
                indexNames,
            );
        }

        // Drop all three indexes for cleanliness; the rst stays up for the caller's tearDown.
        for (const name of indexNames) {
            assert.commandWorked(
                rst.getPrimary().getDB(dbName).getCollection(collName).dropIndex(name),
            );
        }
    }

    /**
     * Drives a single end-to-end "sorter orphan cleanup" scenario for a primary-driven index build.
     *
     * A single-index build is started and paused at `hangAfterIndexBuildSpillBeforeStatePersisted`,
     * which is after the sorter spills to disk but before the metadata about that spill is
     * persisted. Then, after failover, the new primary cleans up these orphaned sorter entries.
     *
     * `skipSpills` selects which branch of `deleteSorterEntriesOutsideRanges` is exercised:
     *   - `skipSpills: 0` — the first spill is orphaned so all sorter entries are deleted and the
     *     scan restarts from the beginning.
     *   - `skipSpills: 1` — the first spill persists its ranges normally and the second spill is
     *     orphaned, so keys outside the persisted range are deleted.
     *
     * Requires a 3-node set (see `_failover`'s UNCLEAN_RESTART path).
     *
     * @param {ReplSetTest} rst - Must have been constructed via `setUp({nodes: 3})`.
     * @param {Object} options
     * @param {number} options.skipSpills - Spills to let persist before the orphaning spill: 0
     *     exercises the "no persisted ranges" branch, 1 the "persisted ranges" branch.
     * @param {string} [options.collName="coll"]
     * @returns {void}
     */
    static runSorterOrphanCleanup(rst, {skipSpills, collName = "coll"}) {
        assert(
            skipSpills === 0 || skipSpills === 1,
            `skipSpills must be 0 or 1, got ${skipSpills}`,
        );
        assert.gte(
            rst.nodes.length,
            TestData.doesNotSupportGracefulStepdown ? 2 : 3,
            "runSorterOrphanCleanup requires a 3-node set, or 2-node set when graceful stepdown " +
                "is not supported)",
        );

        if (
            !PrimaryDrivenResumableIndexBuildTest._featureFlagsEnabled(
                rst.getPrimary().getDB(jsTestName()),
            )
        ) {
            jsTest.log.info(
                "PrimaryDrivenResumableIndexBuildTest: skipping runSorterOrphanCleanup because the " +
                    "primary-driven index build feature flags are disabled",
            );
            return;
        }

        const docCount = 3_000;
        const indexName = "pdib_orphan_a";
        const padBytes = 1500;
        const docTemplate = (i) => ({_id: i, a: String(i).padStart(8, "0") + "x".repeat(padBytes)});

        // Tight memory limit so the first iterations spill quickly.
        PrimaryDrivenResumableIndexBuildTest._setIndexBuildSettings(rst, {
            maxIndexBuildMemoryUsageMegabytes: 1,
            primaryDrivenIndexBuildLoadResumeStateWriteIntervalKeys: 100,
        });

        const dbName = jsTestName();
        const primary = rst.getPrimary();
        const coll = primary.getDB(dbName).getCollection(collName);
        coll.drop();
        bulkInsert(coll, docCount, docTemplate);
        rst.awaitReplication();

        // Pause the build in the window where a spill's data is committed but its ranges are not.
        // `{skip: skipSpills}` selects which spill is orphaned (see method docstring).
        const hangBeforeBuildingIndexFp = configureFailPoint(primary, "hangBeforeBuildingIndex");
        const orphanFp = configureFailPoint(
            primary,
            "hangAfterIndexBuildSpillBeforeStatePersisted",
            {},
            {skip: skipSpills},
        );

        // Kick off the build in a parallel shell.
        const awaitCreateIndexes = startParallelShell(
            funWithArgs(
                function (dbName, collName, indexName) {
                    db.getSiblingDB(dbName).runCommand({
                        createIndexes: collName,
                        indexes: [{key: {a: 1}, name: indexName}],
                    });
                },
                dbName,
                collName,
                indexName,
            ),
            primary.port,
        );

        // Wait for the build to register, then extract its buildUUID.
        let indexes;
        assert.soonNoExcept(function () {
            indexes = IndexBuildTest.assertIndexes(coll, 2, ["_id_"], [indexName], {
                includeBuildUUIDs: true,
            });
            return true;
        });
        const buildUUID = extractUUIDFromObject(indexes[indexName].buildUUID);

        // Wait for the build to reach `hangBeforeBuildingIndex` so the fail point is definitely
        // armed before the build progresses into scan/spill.
        checkLog.containsJson(primary, [4940900, 10978300], {
            buildUUID: function (uuid) {
                return uuid && uuid.uuid && uuid.uuid["$uuid"] === buildUUID;
            },
        });

        rst.awaitLastOpCommitted();
        hangBeforeBuildingIndexFp.off();

        jsTest.log.info(
            "PrimaryDrivenResumableIndexBuildTest: waiting for " +
                `hangAfterIndexBuildSpillBeforeStatePersisted (skipSpills=${skipSpills})`,
        );
        orphanFp.wait();

        // Wait for the secondary to replicate the orphaned spill writes before we uncleanly shut
        // down the primary. The spill's container writes replicate asynchronously, so without this
        // the stepped-up node could win the election without them, leaving the resumed build with
        // no orphans to clean up.
        rst.awaitReplication();

        // Uncleanly shut down the primary, step up the secondary, and then restart the old primary.
        const secondary = rst.getSecondary();
        const newPrimary = PrimaryDrivenResumableIndexBuildTest._failover(
            rst,
            primary,
            secondary,
            PdibFailoverMode.UNCLEAN_RESTART,
        );

        awaitCreateIndexes({checkExitSuccess: false});
        PrimaryDrivenResumableIndexBuildTest._waitForBuildOutcome(
            newPrimary,
            dbName,
            collName,
            [indexName],
            buildUUID,
        );

        checkLog.containsJson(newPrimary, 12784900, {
            numDeleted: function (numDeleted) {
                return numDeleted > 0;
            },
        });
        checkLog.containsJson(newPrimary, 12500800, {phase: "collection scan"});

        rst.awaitReplication();
        for (const node of rst.nodes) {
            const nodeColl = node.getDB(dbName).getCollection(collName);
            IndexBuildTest.assertIndexes(nodeColl, 2, ["_id_", indexName]);
            assert.commandWorked(nodeColl.validate());
        }
        rst.checkReplicatedDataHashes();
    }

    /**
     * @param {DB} db
     * @returns {boolean} True iff `featureFlagContainerWrites`, `featureFlagPrimaryDrivenIndexBuilds` and
     *     `featureFlagResumablePrimaryDrivenIndexBuilds` are enabled.
     */
    static _featureFlagsEnabled(db) {
        return (
            FeatureFlagUtil.isPresentAndEnabled(db, "ContainerWrites") &&
            FeatureFlagUtil.isPresentAndEnabled(db, "PrimaryDrivenIndexBuilds") &&
            FeatureFlagUtil.isPresentAndEnabled(db, "ResumablePrimaryDrivenIndexBuilds")
        );
    }

    /**
     * Applies `setParameter` to every node in `rst`.
     *
     * `setParameter` overrides applied here are stashed on `rst._pdibIndexBuildSettings` so that
     * they can be re-applied later.
     *
     * @param {ReplSetTest} rst
     * @param {Object} params - Key/value pairs forwarded as `setParameter` arguments.
     * @returns {void}
     */
    static _setIndexBuildSettings(rst, params) {
        for (const node of rst.nodes) {
            assert.commandWorked(node.adminCommand({setParameter: 1, ...params}));
        }
        rst._pdibIndexBuildSettings = params;
    }

    /**
     * Defaults to a small handful of side writes so drain has work to do even when callers don't
     * supply any. Keys are well outside the `docCount` range so they never collide with seeded
     * docs.
     *
     * @param {number} docCount
     * @returns {Object[]} A fixed-size array of side-write documents.
     */
    static _defaultSideWrites(docCount) {
        const out = new Array(DEFAULT_SIDE_WRITES_COUNT);
        for (let i = 0; i < DEFAULT_SIDE_WRITES_COUNT; i++) {
            const v = docCount + 1 + i;
            // Each side-write `a` value is distinct (docCount+1 .. docCount+DEFAULT_SIDE_WRITES_COUNT)
            // and is a number, so it never collides with the unique {a:1} index's string keys from
            // defaultDocTemplate — the unique constraint still holds as these drain.
            const doc = {_id: v};
            for (const key of DEFAULT_INDEX_KEYS) {
                doc[key] = v;
            }
            out[i] = doc;
        }
        return out;
    }

    /**
     * Runs one (phase, position) resume scenario: seeds the collection, starts and pauses the
     * build, steps up the secondary, and asserts the resumed build completes with consistent
     * indexes across nodes.
     *
     * @param {ReplSetTest} rst
     * @param {Object} opts
     * @param {string} opts.phase - One of `PdibPhase`.
     * @param {string} opts.position - One of `PdibPosition`.
     * @param {string} opts.failoverMode - One of `PdibFailoverMode`.
     * @param {string} opts.dbName
     * @param {string} opts.collName
     * @param {Object[]} opts.indexSpecs - All three index specs in build order.
     * @param {Function} opts.docTemplate - `(i: number) => Object`.
     * @param {number} opts.docCount
     * @param {Object[]} opts.sideWrites
     * @param {Object[]} opts.postIndexBuildInserts
     * @returns {void}
     */
    static _runOne(rst, opts) {
        const {
            phase,
            position,
            failoverMode,
            dbName,
            collName,
            indexSpecs,
            docTemplate,
            docCount,
            sideWrites,
            postIndexBuildInserts,
        } = opts;

        let primary = rst.getPrimary();
        const coll = primary.getDB(dbName).getCollection(collName);
        coll.drop();

        bulkInsert(coll, docCount, docTemplate);
        rst.awaitReplication();

        const indexNameStem = `${DEFAULT_INDEX_NAME}_${phase.replace(/\s+/g, "_")}_${position}`;
        const indexNames = Array.from(
            {length: DEFAULT_INDEX_COUNT},
            (_, i) => `${indexNameStem}_${i}`,
        );
        const fpInfo = PHASE_FAIL_POINTS[phase];

        // Start the build under hangBeforeBuildingIndex so we can prime the side writes table
        // before the build's actual phases begin.
        const {awaitCreateIndexes, buildUUID, hangBeforeBuildingIndexFp} =
            PrimaryDrivenResumableIndexBuildTest._startBuild(
                rst,
                dbName,
                coll,
                indexSpecs,
                indexNames,
                sideWrites,
            );

        // Configure the phase fail point on the current primary BEFORE releasing the
        // hangBeforeBuildingIndex fail point: otherwise the build can race past the per-phase
        // iteration before the per-phase fail point is set, most importantly for SCAN+BEGINNING
        // (iteration 0). For LOAD/DRAIN the fail point is keyed on the position-selected index.
        const phaseFp = PrimaryDrivenResumableIndexBuildTest._configurePhaseFailPoint(
            primary,
            phase,
            position,
            buildUUID,
            indexNames,
            docCount,
            sideWrites.length,
        );

        // Now release hangBeforeBuildingIndex; the build progresses into the scan and will hit the
        // per-phase fail point we just set.
        hangBeforeBuildingIndexFp.off();

        PrimaryDrivenResumableIndexBuildTest._waitForPause(
            primary,
            fpInfo,
            buildUUID,
            indexNameForPosition(indexNames, position),
        );

        // Snapshot the index_builds.resume.* counters before stepdown so we can assert the resume
        // of *this* build incremented succeeded[<phase>] and didn't bump failed.
        const beforeMetrics = PrimaryDrivenResumableIndexBuildTest._readResumeMetrics(
            rst._pdibMetricsDir,
        );

        // For restart modes, wait for the index build resume state to replicate to secondaries
        // before shutting down the primary. Container writes (including resume state) replicate
        // asynchronously, so without this the stepped-up node could win the election without
        // the resume state, preventing it from resuming the build.
        if (
            failoverMode === PdibFailoverMode.UNCLEAN_RESTART ||
            failoverMode === PdibFailoverMode.CLEAN_RESTART
        ) {
            rst.awaitReplication();
        }

        // Step the secondary up. rst.stepUp() requests an election with a higher term, which
        // forces the old primary to step down even while its build thread is paused at our fail
        // point. This is more deterministic than replSetStepDown + waiting for the secondary to
        // win an election. The awaiter on the parallel shell will receive
        // InterruptedDueToReplStateChange.
        //
        // We must step up before releasing `phaseFp`. Step-up interrupts the build thread via the
        // replication state change; if we instead released the fail point first, the build would
        // simply continue past its current iteration on the old primary and could complete the
        // entire build before we ever step up.
        const oldPrimary = primary;
        const secondary = rst.getSecondary();
        jsTest.log.info(
            `PrimaryDrivenResumableIndexBuildTest: triggering resume via failover mode=${failoverMode}`,
        );
        const newPrimary = PrimaryDrivenResumableIndexBuildTest._failover(
            rst,
            oldPrimary,
            secondary,
            failoverMode,
        );

        // Release the now-stepped-down primary's fail point (cleanup; the build thread has already
        // exited due to the state change). For restart modes the old mongod was recycled, so its
        // in-memory fail point is already gone and the handle's connection is stale — skip. When
        // graceful stepdown is not supported the old primary is always stopped during failover (see
        // `_failOverWithCheckpointInstall`), so its fail point is gone too — skip there as well.
        if (
            failoverMode === PdibFailoverMode.NO_RESTART &&
            !TestData.doesNotSupportGracefulStepdown
        ) {
            phaseFp.off();
        }

        // The shell's createIndexes call against the (now stepped-down) primary returns
        // InterruptedDueToReplStateChange. Don't enforce shell success.
        awaitCreateIndexes({checkExitSuccess: false});

        // Watch for either success (log 20663) or the resume-failure-then-abort path (log 11130400)
        // so a broken resume code path fails fast with a clear message instead of hanging on
        // checkLog's 10-minute default timeout.
        jsTest.log.info(
            `PrimaryDrivenResumableIndexBuildTest: waiting for build ${buildUUID} to complete on new primary`,
        );
        PrimaryDrivenResumableIndexBuildTest._waitForBuildOutcome(
            newPrimary,
            dbName,
            collName,
            indexNames,
            buildUUID,
        );

        rst.awaitReplication();
        PrimaryDrivenResumableIndexBuildTest._verifyIndexAcrossNodes(
            rst,
            dbName,
            collName,
            indexNames,
        );

        // Verify the OTel resume counters moved correctly: exactly one phase attribute on
        // succeeded incremented, that phase is in the expected set for this (PdibPhase,
        // PdibPosition), and failed didn't move.
        PrimaryDrivenResumableIndexBuildTest._verifyResumeMetric(
            rst._pdibMetricsDir,
            beforeMetrics,
            EXPECTED_RESUME_PHASES[phase][position],
        );

        if (postIndexBuildInserts.length > 0) {
            assert.commandWorked(
                newPrimary.getDB(dbName).getCollection(collName).insert(postIndexBuildInserts),
            );
            rst.awaitReplication();
            PrimaryDrivenResumableIndexBuildTest._verifyIndexAcrossNodes(
                rst,
                dbName,
                collName,
                indexNames,
            );
        }

        // Drop all three indexes so the next position starts clean. Leave the collection so the
        // bulk-insert in _runOne has room to skip if a caller wants to reuse data.
        for (const name of indexNames) {
            assert.commandWorked(newPrimary.getDB(dbName).getCollection(collName).dropIndex(name));
        }
    }

    /**
     * Configures the phase fail point on `node` for `(phase, position)`. Returns the fail-point
     * handle so the caller can `.off()` it when done.
     *
     * @param {Mongo} node
     * @param {string} phase - One of `PdibPhase`.
     * @param {string} position - One of `PdibPosition`.
     * @param {string} buildUUID
     * @param {string[]} indexNames
     * @param {number} docCount
     * @param {number} sideWritesCount
     * @returns {Object} Fail-point handle returned by `configureFailPoint()`.
     */
    static _configurePhaseFailPoint(
        node,
        phase,
        position,
        buildUUID,
        indexNames,
        docCount,
        sideWritesCount,
    ) {
        const fpInfo = PHASE_FAIL_POINTS[phase];
        const totalIters = totalIterationsFor(phase, docCount, sideWritesCount);
        const iteration = iterationFor(position, totalIters);
        const fpData = {iteration: NumberLong(iteration)};
        if (fpInfo.matchKey === "buildUUIDs") {
            fpData.buildUUIDs = [buildUUID];
        } else {
            fpData.indexNames = [indexNameForPosition(indexNames, position)];
        }
        return configureFailPoint(node, fpInfo.failPointName, fpData);
    }

    /**
     * Polls the primary's log until the phase fail point's expected log line is observed.
     *
     * @param {Mongo} primary
     * @param {Object} fpInfo - An entry of `PHASE_FAIL_POINTS`.
     * @param {string} fpInfo.failPointName
     * @param {number} fpInfo.logId
     * @param {string} fpInfo.matchKey - `"buildUUIDs"` or `"indexNames"`.
     * @param {string} buildUUID
     * @param {string} indexName
     * @returns {void}
     */
    static _waitForPause(primary, fpInfo, buildUUID, indexName) {
        if (fpInfo.matchKey === "buildUUIDs") {
            checkLog.containsJson(primary, fpInfo.logId, {
                buildUUID: function (uuid) {
                    return uuid && uuid.uuid && uuid.uuid["$uuid"] === buildUUID;
                },
            });
        } else {
            checkLog.containsJson(primary, fpInfo.logId, {index: indexName});
        }
    }

    /**
     * Forces a checkpoint on the primary, waits for the secondary to install it, then kills the
     * primary so the secondary steps up. The build is paused at its phase fail point, so no writes
     * accumulate after the checkpoint. The old primary is always SIGKILLed. Returns the new primary
     * (the stepped-up secondary).
     */
    static _failOverWithCheckpointInstall(rst, {unclean = false} = {}) {
        const oldPrimary = rst.getPrimary();
        const secondary = rst.getSecondary();

        // Force a checkpoint on the primary and block until the secondary installs it.
        PrimaryDrivenResumableIndexBuildTest._awaitCheckpointInstalled(oldPrimary, secondary);

        rst.stop(
            oldPrimary,
            9 /* SIGKILL */,
            {allowedExitCode: MongoRunner.EXIT_SIGKILL},
            {
                forRestart: true,
            },
        );

        jsTest.log.info(
            "PrimaryDrivenResumableIndexBuildTest: killed primary; waiting for secondary to step up",
            {secondary: secondary.host, requestedUnclean: unclean},
        );
        assert.soon(() => {
            try {
                return secondary.adminCommand({hello: 1}).isWritablePrimary;
            } catch (e) {
                return false;
            }
        }, `secondary ${secondary.host} did not step up after the primary was stopped`);

        // Restart the old primary so it rejoins as a secondary (keeps the set at two live nodes).
        const restartedNode = rst.start(oldPrimary, {}, /*restart=*/ true);
        if (rst._pdibIndexBuildSettings) {
            assert.commandWorked(
                restartedNode.adminCommand({setParameter: 1, ...rst._pdibIndexBuildSettings}),
            );
        }
        rst.awaitSecondaryNodes();
        return secondary;
    }

    /**
     * Forces a checkpoint on `primary`, then waits until `secondary` installs that checkpoint.
     *
     * "Drained" means the secondary has adopted a checkpoint at least as new as the primary's last
     * stable checkpoint after the fsync. In the frozen case (a paused build's open transaction pins
     * the stable timestamp) the fsync produces no newer checkpoint and the secondary is already
     * there, so this returns immediately. We also accept the secondary installing any newer
     * checkpoint than before the fsync (it's actively draining), and cap the wait so a missed
     * signal proceeds rather than hangs.
     */
    static _awaitCheckpointInstalled(primary, secondary) {
        const adoptedBefore = PrimaryDrivenResumableIndexBuildTest._checkpointTs(
            secondary,
            "standbyLastAdoptedCheckpointTimestamp",
        );
        assert.commandWorked(primary.adminCommand({fsync: 1}));
        const target = PrimaryDrivenResumableIndexBuildTest._readPrimaryCheckpointTs(primary);
        const haveTarget = timestampCmp(target, Timestamp(0, 0)) > 0;
        jsTest.log.info(
            "PrimaryDrivenResumableIndexBuildTest: forced checkpoint on primary; waiting for secondary " +
                "to drain its ingest tables",
            {secondary: secondary.host, adoptedBefore, primaryCheckpointTimestamp: target},
        );
        const deadline = Date.now() + 2 * 60 * 1000;
        let lastLogged = Date.now();
        for (;;) {
            const adopted = PrimaryDrivenResumableIndexBuildTest._checkpointTs(
                secondary,
                "standbyLastAdoptedCheckpointTimestamp",
            );
            // Done if the secondary reached the primary's post-fsync checkpoint (instant in the
            // frozen case -- the checkpoint didn't advance and the secondary is already there), or it
            // installed any newer checkpoint than before the fsync.
            if (
                (haveTarget && timestampCmp(adopted, target) >= 0) ||
                timestampCmp(adopted, adoptedBefore) > 0
            ) {
                jsTest.log.info(
                    "PrimaryDrivenResumableIndexBuildTest: secondary adopted checkpoint",
                    {secondary: secondary.host, adopted, primaryCheckpointTimestamp: target},
                );
                return;
            }
            if (Date.now() >= deadline) {
                jsTest.log.info(
                    "PrimaryDrivenResumableIndexBuildTest: secondary did not visibly advance within " +
                        "the timeout; treating it as already drained and proceeding",
                    {
                        secondary: secondary.host,
                        adopted,
                        primaryCheckpointTimestamp: target,
                        adoptedBefore,
                    },
                );
                return;
            }
            if (Date.now() - lastLogged >= 30 * 1000) {
                jsTest.log.info(
                    "PrimaryDrivenResumableIndexBuildTest: still waiting for secondary to drain",
                    {
                        secondary: secondary.host,
                        adopted,
                        primaryCheckpointTimestamp: target,
                        adoptedBefore,
                    },
                );
                lastLogged = Date.now();
            }
            sleep(500);
        }
    }

    /**
     * Reads the primary's last stable checkpoint timestamp. That field is best-effort, so retries
     * briefly; returns Timestamp(0, 0) if it never appears.
     */
    static _readPrimaryCheckpointTs(primary) {
        for (let i = 0; i < 40; i++) {
            const ts = PrimaryDrivenResumableIndexBuildTest._checkpointTs(
                primary,
                "primaryCheckpointTimestamp",
            );
            if (timestampCmp(ts, Timestamp(0, 0)) > 0) {
                return ts;
            }
            sleep(50);
        }
        return Timestamp(0, 0);
    }

    static _checkpointTs(node, field) {
        const standby = assert.commandWorked(node.adminCommand({serverStatus: 1})).standby;
        return (standby && standby[field]) || Timestamp(0, 0);
    }

    /**
     * Fails over to the replica set's secondary, returning the new primary.
     */
    static failover(rst) {
        if (TestData.doesNotSupportGracefulStepdown) {
            return PrimaryDrivenResumableIndexBuildTest._failOverWithCheckpointInstall(rst);
        }
        return rst.stepUp(rst.getSecondary());
    }

    /**
     * Triggers a failover according to `failoverMode`.
     *
     * The restart modes require a 3-node (or larger) replica set so the surviving secondary can win
     * the election while the old primary is down — a 2-node set has no quorum once the primary is
     * gone.
     *
     * @param {ReplSetTest} rst
     * @param {Mongo} oldPrimary - The node currently serving as primary, to be displaced.
     * @param {Mongo} nextPrimaryNode - The secondary to step up.
     * @param {string} failoverMode - One of `PdibFailoverMode`.
     * @returns {Mongo} The new primary (may be a fresh connection if a restart was involved).
     */
    static _failover(rst, oldPrimary, nextPrimaryNode, failoverMode) {
        if (TestData.doesNotSupportGracefulStepdown) {
            return PrimaryDrivenResumableIndexBuildTest._failOverWithCheckpointInstall(rst, {
                unclean: failoverMode === PdibFailoverMode.UNCLEAN_RESTART,
            });
        }

        if (failoverMode === PdibFailoverMode.NO_RESTART) {
            return rst.stepUp(nextPrimaryNode);
        }

        // Fail fast on misconfiguration: with only 2 nodes, the surviving secondary can't reach
        // majority after we stop the primary, so `replSetStepUp` would hang in the retry loop.
        assert.gte(
            rst.nodes.length,
            3,
            `failoverMode=${failoverMode} requires a replica set with >= 3 nodes; use setUp({nodes: 3})`,
        );

        const signal =
            failoverMode === PdibFailoverMode.CLEAN_RESTART ? 15 /*SIGTERM*/ : 9; /*SIGKILL*/
        const stopOpts =
            failoverMode === PdibFailoverMode.UNCLEAN_RESTART
                ? {allowedExitCode: MongoRunner.EXIT_SIGKILL}
                : {};
        // 1. Stop the old primary before the step-up.
        rst.stop(oldPrimary, signal, stopOpts, {forRestart: true});

        // 2. Step up the surviving secondary. We can't use `rst.stepUp(...)` here because, even
        //    with `awaitReplicationBeforeStepUp: false`, it follows the `replSetStepUp` command
        //    with `awaitNodesAgreeOnPrimary(this.nodes, ...)`, which hangs waiting for the down
        //    node we just shut down. Send the command directly and poll for `isWritablePrimary`
        //    against the candidate.
        jsTest.log.info(
            `PrimaryDrivenResumableIndexBuildTest: issuing replSetStepUp on ${nextPrimaryNode.host}`,
        );
        assert.soonNoExcept(() => {
            const res = nextPrimaryNode.adminCommand({replSetStepUp: 1});
            return res.ok === 1;
        }, `replSetStepUp on ${nextPrimaryNode.host} did not succeed`);
        assert.soon(() => {
            try {
                return nextPrimaryNode.adminCommand({hello: 1}).isWritablePrimary;
            } catch (e) {
                return false;
            }
        }, `${nextPrimaryNode.host} did not become writable primary`);
        const newPrimary = nextPrimaryNode;

        // 3. Restart the old primary; it rejoins as a secondary.
        const restartedNode = rst.start(oldPrimary, {}, /*restart=*/ true);

        // Re-apply runtime setParameter overrides.
        if (rst._pdibIndexBuildSettings) {
            assert.commandWorked(
                restartedNode.adminCommand({setParameter: 1, ...rst._pdibIndexBuildSettings}),
            );
        }

        // Wait for the restarted node to reach SECONDARY so subsequent getSecondary() /
        // replSetStepUp calls don't race a node still in STARTUP / RECOVERING.
        rst.awaitSecondaryNodes();

        return newPrimary;
    }

    /**
     * Starts a primary-driven index build in a parallel shell, hangs at hangBeforeBuildingIndex,
     * primes the side-writes table, then releases. Returns {awaitCreateIndexes, buildUUID}. The
     * parallel shell's createIndexes is *expected* to be interrupted by step-down, so the caller
     * should drain its awaiter with `{checkExitSuccess: false}`.
     *
     * TODO (SERVER-127026): Deduplicate with `ResumableIndexBuildTest.createIndexesWithSideWrites`.
     *
     * @param {ReplSetTest} rst
     * @param {string} dbName
     * @param {DBCollection} coll
     * @param {Object[]} indexSpecs
     * @param {string[]} indexNames
     * @param {Object[]} sideWrites
     * @returns {{awaitCreateIndexes: Function, buildUUID: string, hangBeforeBuildingIndexFp: Object}}
     *     The caller releases `hangBeforeBuildingIndexFp` after configuring the per-phase fail
     *     point, and drains `awaitCreateIndexes` with `{checkExitSuccess: false}` because
     *     step-down will interrupt it.
     * @see ResumableIndexBuildTest.createIndexesWithSideWrites
     */
    static _startBuild(rst, dbName, coll, indexSpecs, indexNames, sideWrites) {
        assert.eq(indexSpecs.length, DEFAULT_INDEX_COUNT, "_startBuild expects 3 index specs");
        assert.eq(indexNames.length, DEFAULT_INDEX_COUNT, "_startBuild expects 3 index names");
        const primary = rst.getPrimary();
        const fp = configureFailPoint(primary, "hangBeforeBuildingIndex");

        // Each spec is a full createIndexes index doc minus `name` (e.g. {key: {a: 1}, unique: true}
        // or {key: {"$**": 1}}); the test supplies the name. Spread it so per-index options such as
        // `unique` (and any wildcard projection) flow through to the command.
        const indexesArg = indexSpecs.map((spec, i) => ({...spec, name: indexNames[i]}));

        const awaitCreateIndexes = startParallelShell(
            funWithArgs(
                function (dbName, collName, indexes) {
                    // Don't assert.commandWorked: stepdown is expected to interrupt this.
                    db.getSiblingDB(dbName).runCommand({
                        createIndexes: collName,
                        indexes: indexes,
                    });
                },
                dbName,
                coll.getName(),
                indexesArg,
            ),
            primary.port,
        );

        // Wait for the build to register so we can extract its buildUUID.
        let indexes;
        assert.soonNoExcept(function () {
            indexes = IndexBuildTest.assertIndexes(
                coll,
                DEFAULT_INDEX_COUNT + 1,
                ["_id_"],
                indexNames,
                {
                    includeBuildUUIDs: true,
                },
            );
            return true;
        });
        const buildUUID = extractUUIDFromObject(indexes[indexNames[0]].buildUUID);

        // Wait for the build to reach hangBeforeBuildingIndex on the primary.
        checkLog.containsJson(primary, [4940900, 10978300], {
            buildUUID: function (uuid) {
                return uuid && uuid.uuid && uuid.uuid["$uuid"] === buildUUID;
            },
        });

        // Prime the side-writes table while the build is paused.
        if (sideWrites.length > 0) {
            assert.commandWorked(coll.insert(sideWrites));
        }

        // Wait for the last op to be committed so establishing the majority read cursor doesn't
        // race with step-down.
        rst.awaitLastOpCommitted();

        // Return the hangBeforeBuildingIndex fail point so the caller can release it AFTER
        // configuring the per-phase fail point. Releasing here would race the build past iteration
        // 0 of the collection scan before the per-phase fail point is set, causing the
        // SCAN+BEGINNING case to complete without ever hitting the per-phase fail point.
        return {awaitCreateIndexes, buildUUID, hangBeforeBuildingIndexFp: fp};
    }

    /**
     * Waits for the resumed index build to complete or abort on the new primary; throws on abort.
     *
     * Completion: all `indexNames` are ready in `listIndexes`.
     *
     * Abort requires a positive signal — either the build was previously seen in-progress via
     * `listIndexes({includeBuildUUIDs: true})` and has since disappeared, OR abort log 11130400
     * for this `buildUUID` is in the global log. Without it, the post-step-up window where the
     * resumed build is not yet registered would be misread as an abort.
     *
     * Log 20663 is not used as the completion signal: the success line can roll out before the
     * first poll under verbose step-up traffic, causing a spurious timeout.
     *
     * @param {Mongo} newPrimary
     * @param {string} dbName
     * @param {string} collName
     * @param {string[]} indexNames - Names of the indexes the build is creating.
     * @param {string} buildUUID
     * @param {number} [timeoutMs=300000]
     * @returns {void}
     */
    static _waitForBuildOutcome(
        newPrimary,
        dbName,
        collName,
        indexNames,
        buildUUID,
        timeoutMs = 300_000,
    ) {
        const coll = newPrimary.getDB(dbName).getCollection(collName);
        const uuidStr = `"$uuid":"${buildUUID}"`;
        let outcome = null;
        let observedInProgress = false;
        assert.soon(
            () => {
                let ready;
                let inProgress;
                try {
                    ready = new Set(coll.getIndexes().map((idx) => idx.name));
                    inProgress = coll
                        .getIndexes({includeBuildUUIDs: true})
                        .some(
                            (idx) =>
                                idx.buildUUID && extractUUIDFromObject(idx.buildUUID) === buildUUID,
                        );
                } catch (e) {
                    // listIndexes can transiently fail during step-up / state transitions; retry.
                    return false;
                }
                if (inProgress) {
                    observedInProgress = true;
                    return false;
                }
                if (indexNames.every((name) => ready.has(name))) {
                    outcome = "completed";
                    return true;
                }
                // Not in progress and not ready: only conclude abort with a positive signal,
                // otherwise keep polling (see method docstring).
                if (observedInProgress) {
                    outcome = "aborted_after_resume_failure";
                    return true;
                }
                const lines = checkLog.getGlobalLog(newPrimary) ?? [];
                if (lines.some((l) => l.includes('"id":11130400,') && l.includes(uuidStr))) {
                    outcome = "aborted_after_resume_failure";
                    return true;
                }
                return false;
            },
            () =>
                `timed out waiting for build ${buildUUID} to complete or abort on ${newPrimary.host}`,
            timeoutMs,
            1000,
        );
        if (outcome === "aborted_after_resume_failure") {
            // Opportunistically include log 11130400 details if its line is still available —
            // purely informational, the assertion fires either way.
            const lines = checkLog.getGlobalLog(newPrimary) ?? [];
            const abortLine = lines.find(
                (line) => line.includes('"id":11130400,') && line.includes(uuidStr),
            );
            const extra = abortLine ? ` Abort log: ${abortLine}` : "";
            assert(
                false,
                `index build ${buildUUID} was aborted on the new primary instead of resumed. ` +
                    `The resume code path likely failed; check log 12500301 for the ` +
                    `underlying error.${extra}`,
            );
        }
    }

    /**
     * Asserts every named index exists and validates cleanly on every node, and that replicated
     * data hashes match.
     *
     * @param {ReplSetTest} rst
     * @param {string} dbName
     * @param {string} collName
     * @param {string[]} indexNames
     * @returns {void}
     */
    static _verifyIndexAcrossNodes(rst, dbName, collName, indexNames) {
        // Run validate on every node first, then assert if any of them are not valid.
        const results = [];
        for (const node of rst.nodes) {
            const coll = node.getDB(dbName).getCollection(collName);
            IndexBuildTest.assertIndexes(coll, DEFAULT_INDEX_COUNT + 1, ["_id_", ...indexNames]);
            results.push({host: node.host, res: assert.commandWorked(coll.validate())});
        }
        const failures = results.filter(({res}) => !res.valid);
        assert.eq(
            failures.length,
            0,
            `validation failed on ${failures.length}/${results.length} node(s):\n` +
                results.map(({host, res}) => `  ${host}: ${tojson(res)}`).join("\n"),
        );
        rst.checkReplicatedDataHashes();
    }

    /**
     * Reads the OTel JSONL exporter files in `metricsDir` and returns the current totals for
     * index_builds.resume.* counters as:
     *   {succeeded: {<phase>: <count>, ...}, failed: <count>}
     *
     * Unlike getLatestMetrics() in otel_file_export_helpers.js, this preserves the `phase`
     * attribute on succeeded so we can verify the per-phase breakdown. The OTel file exporter
     * writes a fresh snapshot of every counter on each flush, so within a single file we MUST take
     * only the latest record (the cumulative value from that node). Across files we SUM, because
     * each file is a separate node's metrics and a step-up resume on node B is not reflected in
     * node A's counter.
     *
     * TODO (SERVER-127030): Use getLatestMetrics or equivalent and remove this function.
     *
     * @param {string} metricsDir
     * @returns {{succeeded: Object<string, number>, failed: number}} Per-phase succeeded totals
     *     (keyed by the `phase` attribute) and the cluster-wide failed total, summed across
     *     per-node files.
     */
    static _readResumeMetrics(metricsDir) {
        const result = {succeeded: {}, failed: 0};
        const files = findMetricsFiles(metricsDir);
        for (const file of files) {
            const content = cat(file.name);
            if (!content || !content.trim()) continue;
            // Read the last fully-formed JSON line in the file. The exporter appends one record per
            // flush; the last record is the most recent cumulative snapshot. (The very last line
            // can be cut off mid-write, in which case we fall back to the previous one.)
            const lines = content.trim().split("\n");
            let record = null;
            for (let i = lines.length - 1; i >= 0; i--) {
                const line = lines[i].trim();
                if (!line) continue;
                try {
                    record = JSON.parse(line);
                    break;
                } catch (e) {
                    // Try the previous line.
                }
            }
            for (const resourceMetric of record?.resourceMetrics ?? []) {
                for (const scopeMetric of resourceMetric?.scopeMetrics ?? []) {
                    for (const metric of scopeMetric?.metrics ?? []) {
                        if (
                            metric.name !== RESUME_SUCCEEDED_METRIC &&
                            metric.name !== RESUME_FAILED_METRIC
                        ) {
                            continue;
                        }
                        for (const dp of metric.sum?.dataPoints ?? []) {
                            const value = Number(dp.asInt ?? dp.asDouble ?? 0);
                            if (metric.name === RESUME_FAILED_METRIC) {
                                // Sum across nodes (per-file): each file is a separate node's
                                // counter, and we want the cluster-wide total.
                                result.failed += value;
                                continue;
                            }
                            // succeeded counter: split by phase attribute and sum across nodes
                            // (per-file) for the same phase.
                            let phase = null;
                            for (const kv of dp.attributes ?? []) {
                                if (kv.key === "phase") {
                                    phase = kv.value?.stringValue;
                                    break;
                                }
                            }
                            if (phase === null) continue;
                            result.succeeded[phase] = (result.succeeded[phase] ?? 0) + value;
                        }
                    }
                }
            }
        }
        return result;
    }

    /**
     * Asserts the resume counters moved as expected after a single resume:
     *   - failed counter unchanged
     *   - exactly one phase attribute on the succeeded counter incremented by 1
     *   - that phase is in `expectedPhases`
     *
     * Uses assert.soon because the OTel file exporter flushes periodically, so the metric may not
     * be on disk immediately after the resume completes.
     *
     * @param {string} metricsDir
     * @param {{succeeded: Object<string, number>, failed: number}} before - The snapshot returned
     *     by `_readResumeMetrics()` before the resume.
     * @param {string[]} expectedPhases - Server-side phase strings any one of which is acceptable
     *     for the observed increment.
     * @returns {void}
     */
    static _verifyResumeMetric(metricsDir, before, expectedPhases) {
        let last = before;
        assert.soon(
            () => {
                last = PrimaryDrivenResumableIndexBuildTest._readResumeMetrics(metricsDir);
                if (last.failed !== before.failed) return true; // fail fast below
                for (const phase of Object.keys(last.succeeded)) {
                    const delta = (last.succeeded[phase] ?? 0) - (before.succeeded[phase] ?? 0);
                    if (delta > 0) return true;
                }
                return false;
            },
            () =>
                `Timed out waiting for resume metric increment. before=${tojson(before)} last=${tojson(last)}` +
                ` expectedPhases=${tojson(expectedPhases)}`,
            30_000,
            200,
            {runHangAnalyzer: false},
        );

        assert.eq(
            last.failed,
            before.failed,
            `${RESUME_FAILED_METRIC} unexpectedly incremented. before=${tojson(before)} after=${tojson(last)}`,
        );

        const incremented = [];
        let totalDelta = 0;
        for (const phase of Object.keys(last.succeeded)) {
            const delta = (last.succeeded[phase] ?? 0) - (before.succeeded[phase] ?? 0);
            if (delta > 0) {
                incremented.push({phase, delta});
                totalDelta += delta;
            }
        }
        assert.eq(
            totalDelta,
            1,
            `Expected exactly one ${RESUME_SUCCEEDED_METRIC} increment, got ${tojson(incremented)}`,
        );
        const observedPhase = incremented[0].phase;
        assert(
            expectedPhases.includes(observedPhase),
            `${RESUME_SUCCEEDED_METRIC} incremented for phase=${observedPhase}, ` +
                `expected one of ${tojson(expectedPhases)}`,
        );
    }
};
