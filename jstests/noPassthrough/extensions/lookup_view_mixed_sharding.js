/**
 * Tests $lookup, $unionWith, and $graphLookup against a view with a desugaring extension across all
 * four combinations of local/foreign collection sharding. The extensions_* passthroughs cannot
 * express the mixed cases (they shard either everything or nothing), so this test builds its own
 * cluster with extensions loaded.
 *
 * Some combinations are rejected outright by the server, since joins against a sharded foreign
 * collection carry restrictions. Those are checked against an allowlist of documented error codes;
 * only *wrong results* are failures.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionStubParsers,
 *   featureFlagExtensionsInsideHybridSearch,
 *   requires_sharding,
 *   requires_fcv_90,
 * ]
 */
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

// Codes the server may legitimately return when a join's foreign namespace cannot be served in the
// requested topology; failing with one of these is documented behavior, not a bug.
const kUnsupportedJoinCodes = [
    ErrorCodes.NamespaceNotFound,
    28769, // $lookup on a sharded foreign collection (legacy restriction).
    51047,
    28796,
];

const localCollName = "local";
const foreignCollName = "foreign";
const viewName = "foreign_view";

const LOCAL_DOCS = [
    {_id: 1, key: "K"},
    {_id: 2, key: "K"},
];
const FOREIGN_DOCS = [
    {_id: 10, key: "K", name: "a", val: 10},
    {_id: 11, key: "K", name: "b", val: 20},
    {_id: 12, key: "K", name: "c", val: 30},
];

// The view applies a desugaring extension stage, so it yields exactly the 2 lowest-val docs.
const EXPECTED_NAMES = ["a", "b"];

withExtensions(
    {"libmatch_topN_mongo_extension.so": {}},
    (conn, st) => {
        const testDB = conn.getDB(jsTestName());
        const localColl = testDB[localCollName];

        assert.commandWorked(
            testDB.adminCommand({
                enableSharding: testDB.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );

        /**
         * Shards `collName` on _id and moves the chunk above `splitPoint` to shard1, so both shards
         * own data.
         */
        function shardColl(collName, splitPoint) {
            const ns = `${testDB.getName()}.${collName}`;
            assert.commandWorked(testDB.adminCommand({shardCollection: ns, key: {_id: 1}}));
            assert.commandWorked(testDB.adminCommand({split: ns, middle: {_id: splitPoint}}));
            assert.commandWorked(
                testDB.adminCommand({
                    moveChunk: ns,
                    find: {_id: splitPoint},
                    to: st.shard1.shardName,
                }),
            );
        }

        function setup(shardLocal, shardForeign) {
            testDB[viewName].drop();
            testDB[localCollName].drop();
            testDB[foreignCollName].drop();

            assert.commandWorked(testDB[localCollName].insertMany(LOCAL_DOCS));
            assert.commandWorked(testDB[foreignCollName].insertMany(FOREIGN_DOCS));
            if (shardLocal) {
                // Local _ids are 1 and 2, so _id 2 lands on shard1.
                shardColl(localCollName, 2);
            }
            if (shardForeign) {
                // Foreign _ids are 10-12, so _ids 11 and 12 land on shard1.
                shardColl(foreignCollName, 11);
            }
            assert.commandWorked(
                testDB.createView(viewName, foreignCollName, [
                    {$matchTopN: {filter: {}, sort: {val: 1}, limit: 2}},
                ]),
            );
        }

        const OPERATORS = [
            {
                label: "$lookup (pipeline)",
                run: () =>
                    localColl
                        .aggregate([
                            {$match: {_id: 1}},
                            {$lookup: {from: viewName, pipeline: [], as: "j"}},
                        ])
                        .toArray()[0].j,
            },
            {
                label: "$lookup (localField/foreignField)",
                run: () =>
                    localColl
                        .aggregate([
                            {$match: {_id: 1}},
                            {
                                $lookup: {
                                    from: viewName,
                                    localField: "key",
                                    foreignField: "key",
                                    as: "j",
                                },
                            },
                        ])
                        .toArray()[0].j,
            },
            {
                label: "$unionWith",
                run: () =>
                    localColl
                        .aggregate([
                            {$match: {__never_matches__: 1}},
                            {$unionWith: {coll: viewName, pipeline: []}},
                        ])
                        .toArray(),
            },
            {
                label: "$graphLookup",
                run: () =>
                    localColl
                        .aggregate([
                            {$match: {_id: 1}},
                            {
                                $graphLookup: {
                                    from: viewName,
                                    startWith: "$key",
                                    connectFromField: "__none__",
                                    connectToField: "key",
                                    as: "j",
                                },
                            },
                        ])
                        .toArray()[0].j,
            },
        ];

        // Outcome of every combination, logged at the end so the matrix is a readable deliverable.
        const outcomes = [];
        const wrongResults = [];

        for (const shardLocal of [false, true]) {
            for (const shardForeign of [false, true]) {
                setup(shardLocal, shardForeign);
                for (const op of OPERATORS) {
                    const label =
                        `local=${shardLocal ? "sharded" : "unsharded"} ` +
                        `foreign=${shardForeign ? "sharded" : "unsharded"} op=${op.label}`;
                    jsTest.log.info(`Running ${label}`);

                    let docs;
                    try {
                        docs = op.run();
                    } catch (e) {
                        // An "unsupported topology" error is documented server behavior; anything
                        // else is a genuine failure.
                        assert.contains(
                            e.code,
                            kUnsupportedJoinCodes,
                            `${label}: unexpected error from join`,
                            {error: e},
                        );
                        jsTest.log.info(`${label}: unsupported, code ${e.code}`);
                        outcomes.push(`${label} => UNSUPPORTED (code ${e.code})`);
                        continue;
                    }

                    const names = docs.map((d) => d.name).sort();
                    if (friendlyEqual(names, EXPECTED_NAMES)) {
                        outcomes.push(`${label} => OK`);
                    } else {
                        outcomes.push(`${label} => WRONG RESULTS ${tojsononeline(names)}`);
                        wrongResults.push({label, names, docs});
                    }
                }
            }
        }

        jsTest.log.info("Mixed-sharding outcome matrix", {outcomes});
        assert.eq(
            wrongResults.length,
            0,
            "view with a desugaring extension should yield exactly [a, b] for every combination",
            {wrongResults},
        );
    },
    ["sharded"],
    {shards: 2},
);
