/**
 * Test error conditions for the `$search` and `$searchMeta` aggregation stages. These are all
 * mongod-side validation errors, so no search index is needed.
 * E2E version of jstests/with_mongot/search_mocked/search_error_cases.js
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

function runPipeline(pipeline) {
    return db.runCommand({aggregate: collName, pipeline, cursor: {}});
}

describe("$search error cases", function () {
    before(function () {
        coll.drop();
        assert.commandWorked(coll.insertMany([{_id: 0}, {_id: 1}, {_id: 2}]));
    });

    after(function () {
        coll.drop();
    });

    it("should fail inside a transaction", function () {
        const session = db.getMongo().startSession({readConcern: {level: "local"}});
        const sessionDb = session.getDatabase(db.getName());

        session.startTransaction();
        assert.commandFailedWithCode(
            sessionDb.runCommand({aggregate: collName, pipeline: [{$search: {}}], cursor: {}}),
            ErrorCodes.OperationNotSupportedInTransaction,
        );
        session.endSession();
    });

    it("should fail inside a $facet subpipeline", function () {
        assert.commandFailedWithCode(
            runPipeline([{$facet: {originalPipeline: [{$search: {}}]}}]),
            40600,
        );
    });

    it("should fail when not the first stage in a pipeline", function () {
        assert.commandFailedWithCode(runPipeline([{$match: {}}, {$search: {}}]), 40602);
    });

    it("should fail in a findAndModify update pipeline", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                findandmodify: collName,
                // Need a shard-key equality predicate to avoid having the command be implemented
                // as a transaction in sharded scenarios.
                query: {_id: 0},
                update: [{$search: {}}],
            }),
            ErrorCodes.InvalidOptions,
        );
    });

    it("should fail in an update command pipeline", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                update: collName,
                updates: [{q: {_id: 0}, u: [{$search: {}}]}],
            }),
            ErrorCodes.InvalidOptions,
        );
    });

    it("should fail $searchMeta in a findAndModify update pipeline", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                findandmodify: collName,
                query: {_id: 0},
                update: [{$searchMeta: {}}],
            }),
            ErrorCodes.InvalidOptions,
        );
    });

    it("should fail $searchMeta in an update command pipeline", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                update: collName,
                updates: [{q: {_id: 0}, u: [{$searchMeta: {}}]}],
            }),
            ErrorCodes.InvalidOptions,
        );
    });

    it("should reject an oversubscriptionFactor less than 1", function () {
        for (const oversubscriptionFactor of [0.9, 0, -5]) {
            assert.commandFailedWithCode(
                db.adminCommand({
                    setClusterParameter: {internalSearchOptions: {oversubscriptionFactor}},
                }),
                ErrorCodes.BadValue,
            );
        }
    });

    it("should reject a batchSizeGrowthFactor less than 1", function () {
        for (const batchSizeGrowthFactor of [0.9, 0, -5]) {
            assert.commandFailedWithCode(
                db.adminCommand({
                    setClusterParameter: {internalSearchOptions: {batchSizeGrowthFactor}},
                }),
                ErrorCodes.BadValue,
            );
        }
    });
});
