/**
 * Test that $queryStats properly tokenizes update (where the update modification is specified
 * as replacement document or pipeline) commands on mongod.
 *
 * @tags: [featureFlagQueryStatsUpdateCommand]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {getQueryStatsUpdateCmd, resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();

const kHashedFieldName = "lU7Z0mLRPRUL+RfAD5jhYPRRpXBsZBxS/20EzDwfOG4="; // Hash of field "v"
const kHashedIdField = "+0wgDp/AI7f+XT+DJEqixDyZBq9zRe7RGN0wCS9bd94="; // Hash of _id
const kHashedFieldForA = "GDiF6ZEXkeo4kbKyKEAAViZ+2RHIVxBQV9S6b6Lu7gU="; // Hash of field "a"
const kHashedFieldForQ = "S6zZh99V0NXwOj/n5vDnZLcS9d8Fwu1Qdz6ZNDu7++E="; // Hash of field "q"

// Helper to run and validate a single modifier update tokenization test.
function assertModifierUpdateTokenization(testDB, coll, {update, insert, expectedU}) {
    coll.drop();
    assert.commandWorked(coll.insert(insert));

    const cmdObj = {
        update: collName,
        updates: [{q: {v: 1}, u: update}],
    };
    assert.commandWorked(testDB.runCommand(cmdObj));

    let queryStats = getQueryStatsUpdateCmd(testDB, {transformIdentifiers: true});
    assert.eq(1, queryStats.length);
    assert.eq("update", queryStats[0].key.queryShape.command);
    assert.eq({[kHashedFieldName]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);
    assert.eq(expectedU, queryStats[0].key.queryShape.u);
}

/**
 * Runs the test suite against a specific topology.
 *
 * @param {string} topologyName - Name of the topology (e.g. "Standalone", "Sharded")
 * @param {Function} setupFn - Returns {fixture, testDB}
 * @param {Function} teardownFn - Takes {fixture} and cleans up
 */
function runUpdateTokenizationTests(topologyName, setupFn, teardownFn) {
    describe(`update command one way tokenization (${topologyName})`, function () {
        let fixture;
        let testDB;
        let coll;

        before(function () {
            const setupRes = setupFn();
            fixture = setupRes.fixture;
            testDB = setupRes.testDB;
            coll = testDB[collName];

            coll.drop();
            assert.commandWorked(coll.insert({v: 1}));
        });

        after(function () {
            if (fixture) {
                teardownFn(fixture);
            }
        });

        beforeEach(function () {
            resetQueryStatsStore(testDB.getMongo(), "1MB");
        });

        it("should tokenize replacement update correctly", function () {
            const cmdObj = {
                update: collName,
                updates: [{q: {v: 1}, u: {v: 100}}],
                comment: "replacement update!",
            };
            assert.commandWorked(testDB.runCommand(cmdObj));

            let queryStats = getQueryStatsUpdateCmd(testDB, {transformIdentifiers: true});
            assert.eq(1, queryStats.length);
            assert.eq("update", queryStats[0].key.queryShape.command);
            assert.eq({[kHashedFieldName]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);
            assert.eq("?object", queryStats[0].key.queryShape.u);
        });

        // Test that an update filtered on _id is properly tokenized. These _id queries skip
        // parsing during normal update processing (IDHACK optimization), but should still be
        // recorded in query stats.
        it("should tokenize simple _id update correctly", function () {
            coll.drop();
            assert.commandWorked(coll.insert({_id: 123, v: 1}));

            const cmdObj = {
                update: collName,
                updates: [{q: {_id: 123}, u: {_id: 123, v: 100}}],
                comment: "update filtered on _id!",
            };
            assert.commandWorked(testDB.runCommand(cmdObj));

            let queryStats = getQueryStatsUpdateCmd(testDB, {transformIdentifiers: true});
            assert.eq(1, queryStats.length);
            assert.eq("update", queryStats[0].key.queryShape.command);
            assert.eq({[kHashedIdField]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);
            assert.eq("?object", queryStats[0].key.queryShape.u);
        });

        //
        // Modifier update tests - simple operators.
        //

        it("should tokenize $unset modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$unset: {v: 1}},
                insert: {v: 1},
                expectedU: {$unset: {[kHashedFieldName]: 1}},
            });
        });

        it("should tokenize $rename modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$rename: {v: "a"}},
                insert: {v: 1},
                expectedU: {$rename: {[kHashedFieldName]: kHashedFieldForA}},
            });
        });

        it("should tokenize $currentDate modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$currentDate: {v: {$type: "timestamp"}}},
                insert: {v: ISODate("2013-10-02T01:11:18.965Z")},
                expectedU: {$currentDate: {[kHashedFieldName]: {$type: "timestamp"}}},
            });
        });

        //
        // Modifier update tests - arithmetic operators.
        //

        it("should tokenize $inc modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$inc: {v: 5}},
                insert: {v: 1},
                expectedU: {$inc: {[kHashedFieldName]: "?number"}},
            });
        });

        it("should tokenize $min modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$min: {v: 0}},
                insert: {v: 1},
                expectedU: {$min: {[kHashedFieldName]: "?number"}},
            });
        });

        it("should tokenize $max modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$max: {v: 100}},
                insert: {v: 1},
                expectedU: {$max: {[kHashedFieldName]: "?number"}},
            });
        });

        it("should tokenize $mul modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$mul: {v: 2}},
                insert: {v: 1},
                expectedU: {$mul: {[kHashedFieldName]: "?number"}},
            });
        });

        it("should tokenize $bit modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$bit: {v: {and: NumberInt(10)}}},
                insert: {v: NumberInt(1)},
                expectedU: {$bit: {[kHashedFieldName]: {and: "?number"}}},
            });
        });

        //
        // Modifier update tests - array operators.
        //

        it("should tokenize $addToSet modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$addToSet: {v: 99}},
                insert: {v: [1]},
                expectedU: {$addToSet: {[kHashedFieldName]: "?number"}},
            });
        });

        it("should tokenize $addToSet with $each modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$addToSet: {v: {$each: [5, 6, 7]}}},
                insert: {v: [1]},
                expectedU: {$addToSet: {[kHashedFieldName]: {$each: "?array<?number>"}}},
            });
        });

        it("should tokenize $pop modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$pop: {v: -1}},
                insert: {v: [1]},
                expectedU: {$pop: {[kHashedFieldName]: -1}},
            });
        });

        it("should tokenize $push with $each modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$push: {v: {$each: [10]}}},
                insert: {v: [1]},
                expectedU: {$push: {[kHashedFieldName]: {$each: "?array<?number>"}}},
            });
        });

        it("should tokenize $push with $each and $position modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$push: {v: {$each: [10], $position: 0}}},
                insert: {v: [1]},
                expectedU: {$push: {[kHashedFieldName]: {$each: "?array<?number>", $position: "?number"}}},
            });
        });

        it("should tokenize $push with $each and $slice modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$push: {v: {$each: [10], $slice: 1}}},
                insert: {v: [1]},
                expectedU: {$push: {[kHashedFieldName]: {$each: "?array<?number>", $slice: "?number"}}},
            });
        });

        it("should tokenize $push with $each and $sort modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$push: {v: {$each: [10], $sort: {v: 1, a: -1}}}},
                insert: {v: [1]},
                expectedU: {
                    $push: {
                        [kHashedFieldName]: {
                            $each: "?array<?number>",
                            $sort: {[kHashedFieldName]: 1, [kHashedFieldForA]: -1},
                        },
                    },
                },
            });
        });

        it("should tokenize $pull modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$pull: {v: 5}},
                insert: {v: [1]},
                expectedU: {$pull: {[kHashedFieldName]: "?number"}},
            });
        });

        it("should tokenize $pull with $elemMatch modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$pull: {v: {q: {$elemMatch: {a: {$gte: 8}}}}}},
                insert: {v: [1]},
                expectedU: {
                    $pull: {
                        [kHashedFieldName]: {
                            [kHashedFieldForQ]: {$elemMatch: {[kHashedFieldForA]: {$gte: "?number"}}},
                        },
                    },
                },
            });
        });

        // Note that $options is empty in update, hence why when serialized, it is stripped away.
        it("should tokenize $pull with $regex modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$pull: {v: {"$regex": "(?i)a(?-i)bc", "$options": ""}}},
                insert: {v: [1]},
                expectedU: {$pull: {[kHashedFieldName]: {"$regex": "?string"}}},
            });
        });

        it("should tokenize $pullAll modifier update", function () {
            assertModifierUpdateTokenization(testDB, coll, {
                update: {$pullAll: {v: [4, 5, 6]}},
                insert: {v: [1]},
                expectedU: {$pullAll: {[kHashedFieldName]: "?array<?number>"}},
            });
        });

        it("should tokenize pipeline update with all allowed stages", function () {
            const cmdObj = {
                update: collName,
                updates: [
                    {
                        q: {v: 1},
                        u: [{$addFields: {v: 2}}, {$project: {_id: 1}}, {$replaceRoot: {newRoot: {v: 3}}}],
                    },
                ],
                comment: "pipeline update!",
            };
            assert.commandWorked(testDB.runCommand(cmdObj));

            let queryStats = getQueryStatsUpdateCmd(testDB, {transformIdentifiers: true});
            assert.eq(1, queryStats.length);

            assert.eq("update", queryStats[0].key.queryShape.command);
            assert.eq({[kHashedFieldName]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);

            const expectedU = [
                {$addFields: {[kHashedFieldName]: "?number"}},
                {$project: {"+0wgDp/AI7f+XT+DJEqixDyZBq9zRe7RGN0wCS9bd94=": true}},
                {$replaceRoot: {newRoot: "?object"}},
            ];
            assert.eq(expectedU, queryStats[0].key.queryShape.u);
        });

        it("should tokenize pipeline update with stage aliases", function () {
            const cmdObj = {
                update: collName,
                updates: [
                    {
                        q: {v: 1},
                        u: [{$set: {v: "newValue"}}, {$unset: "v"}, {$replaceWith: {newDoc: {v: 5}}}],
                    },
                ],
                comment: "pipeline with stage aliases update!",
            };
            assert.commandWorked(testDB.runCommand(cmdObj));

            let queryStats = getQueryStatsUpdateCmd(testDB, {transformIdentifiers: true});
            assert.eq(1, queryStats.length);
            assert.eq("update", queryStats[0].key.queryShape.command);
            assert.eq({[kHashedFieldName]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);

            const expectedU = [
                {$set: {[kHashedFieldName]: "?string"}},
                // $unset aliases to exclusion $project.
                {$project: {[kHashedFieldName]: false, [kHashedIdField]: true}},
                // $replaceWith aliases to $replaceRoot.
                {$replaceRoot: {newRoot: "?object"}},
            ];
            assert.eq(expectedU, queryStats[0].key.queryShape.u);
        });

        it("should tokenize pipeline update with constants", function () {
            // Hash of "$$newVal"
            const kHashedConstant = "$$VaksLvxpslthbM1qCItU0mhsNVyN8A97qAJzqIKrZoI=";
            const cmdObj = {
                update: collName,
                updates: [
                    {
                        q: {v: 1},
                        u: [{$set: {v: "$$newVal"}}],
                        c: {newVal: 500},
                    },
                ],
                comment: "pipeline update with constants!",
            };
            assert.commandWorked(testDB.runCommand(cmdObj));

            let queryStats = getQueryStatsUpdateCmd(testDB, {transformIdentifiers: true});
            assert.eq(1, queryStats.length);
            assert.eq("update", queryStats[0].key.queryShape.command);
            assert.eq({[kHashedFieldName]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);
            assert.eq([{$set: {[kHashedFieldName]: kHashedConstant}}], queryStats[0].key.queryShape.u);
            assert.eq({newVal: "?number"}, queryStats[0].key.queryShape.c);
        });

        // TODO (SERVER-113907): Add tests for update with array filters.
    });
}

runUpdateTokenizationTests(
    "Standalone",
    () => {
        const conn = MongoRunner.runMongod({setParameter: {internalQueryStatsRateLimit: -1}});
        const testDB = conn.getDB("test");
        testDB[collName].drop();
        return {fixture: conn, testDB};
    },
    (fixture) => MongoRunner.stopMongod(fixture),
);

// TODO SERVER-112050 Enable this when we support sharded clusters for update.
describe.skip("Sharded", function () {
    runUpdateTokenizationTests(
        "Sharded",
        () => {
            const st = new ShardingTest({
                shards: 2,
                mongosOptions: {setParameter: {internalQueryStatsRateLimit: -1}},
            });
            const testDB = st.s.getDB("test");
            st.shardColl(testDB[collName], {_id: 1}, {_id: 1});
            return {fixture: st, testDB};
        },
        (st) => st.stop(),
    );
});
