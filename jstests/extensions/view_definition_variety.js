/**
 * Runs a large variety of view definitions against every join/union operator that can target a
 * view: $lookup (pipeline syntax), $lookup (localField/foreignField syntax), $unionWith, and
 * $graphLookup.
 *
 * The definitions live in libs/view_definition_matrix.js. Because every one preserves `joinKey`,
 * all four operators surface exactly the set of documents the view yields, so one expectation
 * drives the whole matrix.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionStubParsers,
 *   featureFlagExtensionsInsideHybridSearch,
 *   requires_fcv_90,
 * ]
 */
import {
    FOREIGN_DOCS,
    LOCAL_DOCS,
    SIDE_DOCS,
    getUserPipelines,
    getViewDefs,
} from "jstests/extensions/libs/view_definition_matrix.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {afterEach, before, describe, it} from "jstests/libs/mochalite.js";

const localCollName = jsTestName() + "_local";
const foreignCollName = jsTestName() + "_foreign";
const sideCollName = jsTestName() + "_side";

// Must match the number of entries in getViewDefs(); the assert below catches a mismatch.
const NUM_VIEW_DEFS = 16;

// Operators that can target a view. Each returns the set of documents the view yielded, so the
// same expectation applies to all of them.
const OPERATORS = [
    {
        label: "$lookup (pipeline syntax)",
        isLookup: true,
        // An empty subpipeline carries no joinKey predicate, so it also surfaces the joinKey-less
        // side-collection docs one view definition unions in; filtering them keeps joinKey
        // semantics uniform across the four operators without hiding extra view output.
        takesSubpipeline: true,
        run: (localColl, viewName, sub) =>
            localColl
                .aggregate([{$lookup: {from: viewName, pipeline: sub, as: "j"}}])
                .toArray()[0]
                .j.filter((d) => d.joinKey === "K"),
    },
    {
        label: "$lookup (localField/foreignField syntax)",
        isLookup: true,
        run: (localColl, viewName) =>
            localColl
                .aggregate([
                    {
                        $lookup: {
                            from: viewName,
                            localField: "key",
                            foreignField: "joinKey",
                            as: "j",
                        },
                    },
                ])
                .toArray()[0].j,
    },
    {
        label: "$unionWith",
        takesSubpipeline: true,
        run: (localColl, viewName, sub) =>
            localColl
                .aggregate([
                    {$match: {__never_matches__: 1}},
                    {$unionWith: {coll: viewName, pipeline: sub}},
                    // Drop the side-collection docs that one view definition unions in; they have
                    // no joinKey, matching the joinKey-based semantics of the other operators.
                    {$match: {joinKey: "K"}},
                ])
                .toArray(),
    },
    {
        label: "$graphLookup",
        run: (localColl, viewName) =>
            localColl
                .aggregate([
                    {
                        $graphLookup: {
                            from: viewName,
                            startWith: "$key",
                            connectFromField: "__none__",
                            connectToField: "joinKey",
                            as: "j",
                        },
                    },
                ])
                .toArray()[0].j,
    },
];

describe("view definition variety across join operators", function () {
    let localColl, foreignColl, sideColl;
    let viewDefs;
    let createdViews;

    before(function () {
        localColl = db[localCollName];
        foreignColl = db[foreignCollName];
        sideColl = db[sideCollName];
        localColl.drop();
        foreignColl.drop();
        sideColl.drop();
        assert.commandWorked(localColl.insertMany(LOCAL_DOCS));
        assert.commandWorked(foreignColl.insertMany(FOREIGN_DOCS));
        assert.commandWorked(sideColl.insertMany(SIDE_DOCS));
        viewDefs = getViewDefs(sideCollName);
        createdViews = [];
    });

    afterEach(function () {
        for (const viewName of createdViews.reverse()) {
            assertDropCollection(db, viewName);
        }
        createdViews = [];
    });

    // One `it` per (view definition, operator, user-pipeline size) so a failure names all three
    // dimensions. $lookup's localField/foreignField syntax and $graphLookup take no user
    // subpipeline, so they run once instead of once per size.
    for (let i = 0; i < NUM_VIEW_DEFS; i++) {
        for (const operator of OPERATORS) {
            const userPipes = operator.takesSubpipeline
                ? getUserPipelines()
                : [{label: "no", pipeline: [], marker: null}];
            for (const userPipe of userPipes) {
                it(`view def #${i} with ${operator.label} and ${userPipe.label} user pipeline`, function () {
                    const def = viewDefs[i];
                    assert(def, `view def #${i} missing from the table`);

                    if (def.skipReason) {
                        jsTest.log.info(
                            `SKIPPING view def #${i} ("${def.label}") with ${operator.label}`,
                            {skipReason: def.skipReason},
                        );
                        return;
                    }

                    const viewName = jsTestName() + "_vdef_" + i;
                    assert.commandWorked(db.createView(viewName, foreignCollName, def.pipeline));
                    createdViews.push(viewName);

                    const context =
                        `view def "${def.label}" via ${operator.label} with ` +
                        `${userPipe.label} user pipeline`;

                    // Definitions marked 'lookupFailsWithCode' are unusable under $lookup on a
                    // standalone or replica set: the prepended foreign source displaces the view's
                    // source stage. On a sharded cluster the view resolves correctly for both
                    // $lookup syntaxes.
                    if (def.lookupFailsWithCode && operator.isLookup) {
                        if (!FixtureHelpers.isMongos(db)) {
                            const err = assert.throws(
                                () => operator.run(localColl, viewName, userPipe.pipeline),
                                [],
                                `${context}: expected failure ${def.lookupFailsWithCode}`,
                            );
                            jsTest.log.info(`${context}: lookupGap error`, {
                                code: err.code,
                                message: String(err.message),
                            });
                            return;
                        }
                    }

                    const docs = operator.run(localColl, viewName, userPipe.pipeline);

                    if (def.expectedNames !== undefined) {
                        const actualNames = docs.map((d) => d.name).sort();
                        assert.eq(actualNames, def.expectedNames, context, {docs});
                    } else {
                        assert.eq(docs.length, def.expectedCount, `${context} document count`, {
                            docs,
                        });
                    }

                    // A dropped view prefix is invisible whenever the definition is
                    // result-preserving, so check the marker directly.
                    if (def.marker) {
                        for (const doc of docs) {
                            assert.eq(
                                doc[def.marker],
                                true,
                                `${context}: view pipeline must apply`,
                                {
                                    doc,
                                },
                            );
                        }
                    }

                    // ... and so must the user subpipeline, when it has one.
                    if (userPipe.marker) {
                        for (const doc of docs) {
                            assert.eq(
                                doc[userPipe.marker],
                                true,
                                `${context}: user pipeline must run`,
                                {doc},
                            );
                        }
                    }
                });
            }
        }
    }
});
