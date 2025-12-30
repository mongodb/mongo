/**
 * This test confirms that query stats store key fields for modifier update commands
 * are properly nested and none are missing.
 *
 * @tags: [featureFlagQueryStatsUpdateCommand]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryStats,
    queryShapeUpdateFieldsRequired,
    resetQueryStatsStore,
    runCommandAndValidateQueryStats,
    updateKeyFieldsComplex,
    updateKeyFieldsRequired,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();

/**
 * Runs the test suite against a specific topology.
 *
 * @param {string} topologyName - Name of the topology (e.g. "Standalone", "Sharded")
 * @param {Function} setupFn - Returns {fixture, testDB}
 * @param {Function} teardownFn - Takes {fixture} and cleans up
 */
function runModifierUpdateKeyTests(topologyName, setupFn, teardownFn) {
    describe(`query stats modifier update key (${topologyName})`, function () {
        let fixture;
        let testDB;
        let coll;

        before(function () {
            const setupRes = setupFn();
            fixture = setupRes.fixture;
            testDB = setupRes.testDB;
            coll = testDB[collName];

            // Have to create an index for hint not to fail.
            assert.commandWorked(coll.createIndex({v: 1}));
        });

        after(function () {
            if (fixture) {
                teardownFn(fixture);
            }
        });

        it("should validate simple modifier update key fields", function () {
            const modifierUpdateCommandObjSimple = {
                update: collName,
                updates: [{q: {v: 3}, u: {$set: {v: 4, modifierUpdated: true}}}],
            };

            runCommandAndValidateQueryStats({
                coll: coll,
                commandName: "update",
                commandObj: modifierUpdateCommandObjSimple,
                shapeFields: queryShapeUpdateFieldsRequired,
                keyFields: updateKeyFieldsRequired,
            });
        });

        it("should validate complex modifier update key fields", function () {
            const queryShapeModifierUpdateFieldsComplex = [...queryShapeUpdateFieldsRequired, "collation"];

            // Test with all possible update modifier operators.
            const modifierUpdateCommandObjComplex = {
                update: collName,
                updates: [
                    {
                        q: {v: {$gt: 5}},
                        u: {
                            $set: {
                                item: "ABC123",
                                "info.publisher": "2222",
                                tags: ["software"],
                                "ratings.1": {by: "xyz", rating: 3},
                            },
                            $unset: {tagsToRemove: 1},
                            $rename: {oldName: "newName"},
                            $setOnInsert: {newInsert: true},
                            $currentDate: {lastModified: {$type: "timestamp"}},
                            $bit: {expdata: {and: NumberInt(10)}},
                            $min: {minPrice: 5},
                            $max: {maxPrice: 500},
                            $mul: {quantity: 2},
                            $addToSet: {
                                scores: {
                                    $each: [50, 60, 70],
                                },
                            },
                            $push: {
                                scoresSingleAdd: 89,
                                tests: {$each: [40, 60], $sort: 1},
                                scoresWithPostion: {$each: [50, 60, 70], $position: 0},
                                scoresToSlice: {$each: [80, 78, 86], $slice: -5},
                                quizzes: {
                                    $each: [
                                        {id: 3, score: 8},
                                        {id: 4, score: 7},
                                        {id: 5, score: 6},
                                    ],
                                    $sort: {score: 1},
                                },
                            },
                            $pop: {tagsToPop: -1},
                            $pull: {
                                instock: {$elemMatch: {qty: {$gt: 10, $lte: 20}}},
                                pulledObjects: {testField: 6},
                                arrayToPullFrom: 6,
                                results: {answers: {$elemMatch: {q: 2, a: {$gte: 8}}}},
                                resultsWithoutPredicate: {q: 2, a: 8},
                                "where.to.begin": {"$regex": "^thestart", "$options": ""},
                            },
                            $pullAll: {colorsToRemove: ["red", "blue"]},
                        },
                        multi: true,
                        upsert: false,
                        collation: {locale: "en_US", strength: 2},
                        hint: {"v": 1},
                    },
                ],
                ordered: false,
                bypassDocumentValidation: true,
                comment: "modifier update test!!!",
                readConcern: {level: "local"},
                maxTimeMS: 50 * 1000,
                apiDeprecationErrors: false,
                apiVersion: "1",
                apiStrict: false,
                $readPreference: {"mode": "primary"},
            };

            runCommandAndValidateQueryStats({
                coll: coll,
                commandName: "update",
                commandObj: modifierUpdateCommandObjComplex,
                shapeFields: queryShapeModifierUpdateFieldsComplex,
                keyFields: updateKeyFieldsComplex,
            });
        });

        // TODO (SERVER-113907): Add tests for update with array filters.
        // For now, this is a negative test to ensure that updates with array filters are skipped.
        it("should validate modifier update with array filters", function () {
            const modifierUpdateCommandObjSimple = {
                update: collName,
                updates: [{q: {v: 3}, u: {$set: {"myArray.$[element]": 10}}, arrayFilters: [{element: 0}]}],
            };

            resetQueryStatsStore(testDB.getMongo(), "1MB");
            assert.commandWorked(testDB.runCommand(modifierUpdateCommandObjSimple));
            let sortedEntries = getQueryStats(
                testDB.getMongo(),
                Object.merge({customSort: {"metrics.latestSeenTimestamp": -1}}, {collName: coll.getName()}),
            );
            assert.eq([], sortedEntries);
        });

        it("should validate modifier update with no-op operators", function () {
            const modifierUpdateCommandObjNoop = {
                update: collName,
                updates: [
                    {
                        q: {v: 3},
                        u: {
                            $set: {},
                            $unset: {},
                            $rename: {},
                            $setOnInsert: {},
                            $currentDate: {},
                            $bit: {},
                            $min: {},
                            $max: {},
                            $mul: {},
                            $addToSet: {},
                            $push: {},
                            $pop: {},
                            $pull: {},
                            $pullAll: {},
                        },
                    },
                ],
            };

            runCommandAndValidateQueryStats({
                coll: coll,
                commandName: "update",
                commandObj: modifierUpdateCommandObjNoop,
                shapeFields: queryShapeUpdateFieldsRequired,
                keyFields: updateKeyFieldsRequired,
            });
        });

        it("should validate modifier update on dollar-prefixed fields", function () {
            // Similar query to modifierUpdateCommandObjComplex but with a $ prefix on every field name where one should be supported
            const modifierUpdateCommandObjComplexDollarPrefix = {
                update: collName,
                updates: [
                    {
                        q: {v: {$gt: 5}},
                        u: {
                            $set: {
                                "$item": "ABC123",
                                "$info.publisher": "2222",
                                "info.$publisher2": "3333",
                                "$tags": ["software"],
                                "ratings.1": {by: "xyz", "$rating": 3},
                                "ratings.$2": {by: "uvw", rating: 4},
                            },
                            $unset: {"$tagsToRemove": 1},
                            $rename: {"$oldName": "newName"},
                            $setOnInsert: {"$newInsert": true},
                            $currentDate: {"$lastModified": {$type: "timestamp"}},
                            $bit: {"$expdata": {and: NumberInt(10)}},
                            $min: {"$minPrice": 5},
                            $max: {"$maxPrice": 500},
                            $mul: {"$quantity": 2},
                            $addToSet: {
                                "$scores": {
                                    $each: [50, 60, 70],
                                },
                            },
                            $push: {
                                "$scoresSingleAdd": 89,
                                "$tests": {$each: [40, 60], $sort: 1},
                                "$scoresWithPostion": {$each: [50, 60, 70], $position: 0},
                                "$scoresToSlice": {$each: [80, 78, 86], $slice: -5},
                                "$quizzes": {
                                    $each: [
                                        {id: 3, "$score": 8},
                                        {id: 4, "$score": 7},
                                        {id: 5, "$score": 6},
                                    ],
                                    $sort: {"$score": 1},
                                },
                            },
                            $pop: {"$tagsToPop": -1},
                            $pull: {
                                "$instock": {$elemMatch: {qty: {$gt: 10, $lte: 20}}},
                                "$pulledObjects": {testField: 6},
                                "$arrayToPullFrom": 6,
                                "$results": {answers: {$elemMatch: {q: 2, a: {$gte: 8}}}}, // note: $answers isn't expected to work here
                                "$resultsWithoutPredicate": {q: 2, a: 8},
                                "where.to.$begin": {"$regex": "^thestart", "$options": ""},
                            },
                            $pullAll: {"$colorsToRemove": ["red", "blue"]},
                        },
                    },
                ],
            };

            runCommandAndValidateQueryStats({
                coll: coll,
                commandName: "update",
                commandObj: modifierUpdateCommandObjComplexDollarPrefix,
                shapeFields: queryShapeUpdateFieldsRequired,
                keyFields: updateKeyFieldsRequired,
            });
        });
    });
}

runModifierUpdateKeyTests(
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
    runModifierUpdateKeyTests(
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
