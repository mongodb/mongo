/**
 * This test confirms that query stats store key fields for modifier update commands
 * are properly nested and none are missing.
 *
 * @tags: [featureFlagQueryStatsUpdateCommand]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    runCommandAndValidateQueryStats,
    withQueryStatsEnabled,
    queryShapeUpdateFieldsRequired,
    updateKeyFieldsRequired,
    updateKeyFieldsComplex,
    getQueryStats,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();

function testModifierUpdateSimple(coll) {
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
}

function testModifierUpdateComplex(coll) {
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
}

// TODO (SERVER-113907): Add tests for update with array filters.
// For now, this is a negative test to ensure that updates with array filters are skipped.
function testModifierUpdateWithArrayFilters(db, coll) {
    const modifierUpdateCommandObjSimple = {
        update: collName,
        updates: [{q: {v: 3}, u: {$set: {"myArray.$[element]": 10}}, arrayFilters: [{element: 0}]}],
    };

    resetQueryStatsStore(db.getMongo(), "1MB");
    assert.commandWorked(db.runCommand(modifierUpdateCommandObjSimple));
    let sortedEntries = getQueryStats(
        db.getMongo(),
        Object.merge({customSort: {"metrics.latestSeenTimestamp": -1}}, {collName: coll.getName()}),
    );
    assert.eq([], sortedEntries);
}

withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();

    if (testDB.getMongo().isMongos()) {
        // TODO SERVER-112050 Unskip this when we support sharded clusters for update.
        jsTest.log.info("Skipping update key validation test on sharded cluster.");
        return;
    }

    // Have to create an index for hint not to fail.
    assert.commandWorked(coll.createIndex({v: 1}));

    testModifierUpdateSimple(coll);
    testModifierUpdateComplex(coll);
    testModifierUpdateWithArrayFilters(testDB, coll);
});
