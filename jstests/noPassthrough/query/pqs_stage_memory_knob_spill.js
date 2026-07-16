/**
 * End-to-end coverage that every stage-memory-limit knob settable via QuerySettings (pqs_settable)
 * actually takes effect at execution time. Both the target execution engine ('queryFramework') and
 * the knob override are set via setQuerySettings on the query shape, so no global state is mutated.
 *
 * Lowering the knob to a tiny value must change the stage's behaviour: it either spills to disk or
 * fails with a memory-limit error (some stages spill, others error when they can't). At the default
 * limit -- same engine, no knob override -- the query neither spills nor errors. That contrast
 * proves the override, not ambient behaviour, is responsible.
 *
 * @tags: [
 *   featureFlagPqsQueryKnobs,
 *   requires_fcv_90,
 *   # Spilling record stores report the correct storage size only on persistent storage.
 *   requires_persistence,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Returns true if any node in the explain output reports having spilled to disk. Different stages
// and engines expose different fields ('spills', 'spilledRecords', 'usedDisk'), so check all.
function didSpill(explain) {
    let spilled = false;
    (function walk(node) {
        if (Array.isArray(node)) {
            node.forEach(walk);
            return;
        }
        if (node && typeof node === "object") {
            for (const [key, value] of Object.entries(node)) {
                if ((key === "spills" || key === "spilledRecords") && value > 0) {
                    spilled = true;
                } else if (key === "usedDisk" && value === true) {
                    spilled = true;
                } else {
                    walk(value);
                }
            }
        }
    })(explain);
    return spilled;
}

describe("QuerySettings stage-memory knob overrides take effect", function () {
    before(function () {
        // Query settings are stored as a cluster parameter, which requires a replica set;
        // withQuerySettings() does not work against a standalone.
        this.rst = new ReplSetTest({nodes: 1});
        this.rst.startSet();
        this.rst.initiate();
        this.db = this.rst.getPrimary().getDB("test");
        // Debug/sanitizer variants force extra spilling regardless of the memory limit, which
        // breaks the baseline: the SBE HashAgg spilling mode defaults to "inDebug", and sanitizer
        // variants start mongod with internalQueryEnableAggressiveSpillsInGroup: true. Disable both
        // so each $group case spills only when its knob override is exceeded.
        assert.commandWorked(
            this.db.adminCommand({
                setParameter: 1,
                internalQuerySlotBasedExecutionHashAggIncreasedSpilling: "never",
                internalQueryEnableAggressiveSpillsInGroup: false,
            }),
        );
        this.coll = this.db[jsTestName()];
        this.coll.drop();
        assert.commandWorked(this.coll.createIndex({loc: "2dsphere"}));
        assert.commandWorked(this.coll.createIndex({txt: "text"}));

        const bigStr = "x".repeat(1024);
        const bulk = this.coll.initializeUnorderedBulkOp();
        for (let i = 0; i < 1000; i++) {
            bulk.insert({
                _id: i,
                g: i % 50,
                n: i,
                bigStr,
                txt: `term${i % 20} shared`,
                loc: {type: "Point", coordinates: [(i % 90) / 10, (i % 90) / 10]},
            });
        }
        assert.commandWorked(bulk.execute());

        this.qsutils = new QuerySettingsUtils(this.db, this.coll.getName());
        this.collName = this.coll.getName();
    });

    after(function () {
        this.rst.stopSet();
    });

    // Each entry stresses one stage backed by a pqs_settable stage-memory knob. 'engine' selects the
    // execution engine that reads that knob; 'knob' is the wire name; 'pipeline' spills once the
    // knob is lowered to a tiny value.
    const cases = [
        {
            name: "$group (classic)",
            engine: "classic",
            knob: "documentSourceGroupMaxMemoryBytes",
            pipeline: [{$group: {_id: "$g", c: {$sum: "$n"}}}],
        },
        {
            name: "$group (SBE hashagg)",
            engine: "sbe",
            knob: "sbeHashAggApproxMemoryUseInBytesBeforeSpill",
            pipeline: [{$group: {_id: "$g", c: {$sum: "$n"}}}],
        },
        {
            name: "$graphLookup",
            engine: "classic",
            knob: "documentSourceGraphLookupMaxMemoryBytes",
            pipeline: (c) => [
                {
                    $graphLookup: {
                        from: c,
                        startWith: "$g",
                        connectFromField: "g",
                        connectToField: "g",
                        as: "out",
                    },
                },
            ],
        },
        {
            name: "$setWindowFields",
            engine: "classic",
            knob: "documentSourceSetWindowFieldsMaxMemoryBytes",
            pipeline: [
                {
                    $setWindowFields: {
                        sortBy: {n: 1},
                        output: {
                            vals: {$push: "$bigStr", window: {documents: ["unbounded", "current"]}},
                        },
                    },
                },
            ],
        },
        {
            name: "$bucketAuto",
            engine: "classic",
            knob: "documentSourceBucketAutoMaxMemoryBytes",
            pipeline: [
                {$bucketAuto: {groupBy: "$n", buckets: 5, output: {vals: {$push: "$bigStr"}}}},
            ],
        },
        {
            name: "$densify",
            engine: "classic",
            knob: "documentSourceDensifyMaxMemoryBytes",
            pipeline: [{$densify: {field: "n", range: {step: 1, bounds: "full"}}}],
        },
        {
            name: "$lookup (SBE hash lookup)",
            engine: "sbe",
            knob: "sbeHashLookupApproxMemoryUseInBytesBeforeSpill",
            pipeline: (c) => [{$lookup: {from: c, localField: "g", foreignField: "g", as: "out"}}],
        },
        {
            name: "near",
            engine: "classic",
            knob: "nearStageMaxMemoryBytes",
            pipeline: [
                {
                    $geoNear: {
                        near: {type: "Point", coordinates: [0, 0]},
                        distanceField: "d",
                        spherical: true,
                    },
                },
            ],
        },
        {
            // Projecting {$meta: "textScore"} makes the planner build the blocking TEXT_OR stage
            // (it must fetch and score every matching record), which is what reads the knob.
            name: "text $or",
            engine: "classic",
            knob: "textOrStageMaxMemoryBytes",
            pipeline: [
                {$match: {$text: {$search: "shared"}}},
                {$project: {score: {$meta: "textScore"}}},
            ],
        },
    ];

    // Runs the pipeline under executionStats explain and reports whether it succeeded and, if so,
    // whether any stage spilled. A memory-limit failure is reported as {ok: false}.
    function runOutcome(coll, pipeline) {
        try {
            return {
                ok: true,
                spilled: didSpill(coll.explain("executionStats").aggregate(pipeline)),
            };
        } catch (e) {
            return {ok: false, spilled: false, code: e.code};
        }
    }

    for (const tc of cases) {
        it(`${tc.name}: setQuerySettings knob override changes execution behaviour`, function () {
            const pipeline =
                typeof tc.pipeline === "function" ? tc.pipeline(this.collName) : tc.pipeline;
            const representativeQuery = this.qsutils.makeAggregateQueryInstance({pipeline});
            const run = () => runOutcome(this.coll, pipeline);

            // Baseline: same engine, default knob value -> the query completes without spilling.
            this.qsutils.withQuerySettings(representativeQuery, {queryFramework: tc.engine}, () => {
                const base = run();
                assert(
                    base.ok && !base.spilled,
                    `${tc.name} spilled or failed at the default limit`,
                    base,
                );
            });

            // Lowering the knob (same engine) must change behaviour: the stage either spills or
            // fails with a memory-limit error.
            const settings = {queryFramework: tc.engine, queryKnobs: {[tc.knob]: NumberLong(1)}};
            this.qsutils.withQuerySettings(representativeQuery, settings, () => {
                const overridden = run();
                assert(
                    overridden.spilled || !overridden.ok,
                    `${tc.name} neither spilled nor failed with the knob override`,
                    overridden,
                );
            });
        });
    }
});
