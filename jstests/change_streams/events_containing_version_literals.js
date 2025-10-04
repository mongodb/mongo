/**
 * Tests that change stream events containg a '$v' field work as expected.
 * @tags: [
 *   uses_change_streams,
 * ]
 */

import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

describe("change streams correctly return documents containing $v attributes", () => {
    const kCollName = jsTestName();
    let cst;
    let cursor;

    beforeEach(() => {
        assertDropAndRecreateCollection(db, kCollName);

        cst = new ChangeStreamTest(db);
        cursor = cst.startWatchingChanges({
            pipeline: [{$changeStream: {}}],
            collection: db[kCollName],
        });

        // Insert 5 documents that will be used in the following update tests.
        [1, 2, "1", "2", "test"].forEach((v, i) => {
            assert.commandWorked(db[kCollName].insert({_id: i, $v: v}));
            let expected = {
                documentKey: {_id: i},
                fullDocument: {_id: i, $v: v},
                ns: {db: "test", coll: kCollName},
                operationType: "insert",
            };
            cst.assertNextChangesEqual({cursor, expectedChanges: [expected]});
        });
    });

    afterEach(() => {
        cst.cleanUp();
    });

    // Test update operations using $v field name, and check that they are either
    // We update in different order here than update, so that we don't cause no-op updates.
    it("tests that updates using $v inside an object literal fail", () => {
        ["test", 1, 2, "1", "2"].forEach((v, i) => {
            assert.commandFailedWithCode(db[kCollName].update({_id: i}, {$v: v}), ErrorCodes.FailedToParse);
        });
    });

    it("tests that updates using $v inside an object literal succeed with upsert when the source documents do not exist", () => {
        ["test", 1, 2, "1", "2"].forEach((v, i) => {
            assert.commandFailedWithCode(
                db[kCollName].update({_id: i + 10}, {$v: v}, {upsert: true}),
                ErrorCodes.FailedToParse,
            );
        });
    });

    it("tests that updates using $v inside $set and an object literal fail", () => {
        ["test", 1, 2, "1", "2"].forEach((v, i) => {
            assert.commandFailedWithCode(
                db[kCollName].update({_id: i}, {$set: {$v: v}}),
                ErrorCodes.DollarPrefixedFieldName,
            );
        });
    });

    it("tests that updates using $v inside $set and an object literal succeed with upsert when the source documents do not exist", () => {
        // Target documents do not yet exist, so the upsert creates them using inserts.
        ["test", 1, 2, "1", "2"].forEach((v, i) => {
            assert.commandWorked(db[kCollName].update({_id: i + 10}, {$set: {$v: v}}, {upsert: true}));
            let expected = {
                documentKey: {_id: i + 10},
                fullDocument: {_id: i + 10, $v: v},
                ns: {db: "test", coll: kCollName},
                operationType: "insert",
            };
            cst.assertNextChangesEqual({cursor, expectedChanges: [expected]});
        });
    });

    it("tests that pipeline updates using $v inside $replaceWith and $literal succeed", () => {
        ["test", 1, 2, "1", "2"].forEach((v, i) => {
            assert.commandWorked(db[kCollName].update({_id: i}, [{$replaceWith: {$literal: {_id: i, $v: v}}}]));
            let expected = {
                documentKey: {_id: i},
                fullDocument: {_id: i, $v: v},
                ns: {db: "test", coll: kCollName},
                operationType: "replace",
            };
            cst.assertNextChangesEqual({cursor, expectedChanges: [expected]});
        });
    });

    it("tests that pipeline updates using $v inside using $replaceWith and an object literal fail", () => {
        ["test", 1, 2, "1", "2"].forEach((v, i) => {
            assert.commandFailedWithCode(db[kCollName].update({_id: i}, [{$replaceWith: {_id: i, $v: v}}]), 16410);
        });
    });

    it("tests that pipeline updates using $v inside using $replaceRoot and $literal succeed", () => {
        ["test", 1, 2, "1", "2"].forEach((v, i) => {
            assert.commandWorked(
                db[kCollName].update({_id: i}, [{$replaceRoot: {newRoot: {$literal: {_id: i, $v: v}}}}]),
            );
            let expected = {
                documentKey: {_id: i},
                fullDocument: {_id: i, $v: v},
                ns: {db: "test", coll: kCollName},
                operationType: "replace",
            };
            cst.assertNextChangesEqual({cursor, expectedChanges: [expected]});
        });
    });

    it("tests that pipeline updates using $v using $replaceRoot and an object literal fail", () => {
        ["test", 1, 2, "1", "2"].forEach((v, i) => {
            assert.commandFailedWithCode(
                db[kCollName].update({_id: i}, [{$replaceRoot: {newRoot: {_id: i, $v: v}}}]),
                16410,
            );
        });
    });

    it("tests that update using $v inside $inc fails", () => {
        ["test", 1, 2, "1", "2"].forEach((v, i) => {
            assert.commandFailedWithCode(db[kCollName].update({_id: i}, {$inc: {$v: 1}}), [
                ErrorCodes.DollarPrefixedFieldName,
                ErrorCodes.TypeMismatch,
            ]);
        });
    });

    it("tests that updates using $v inside $addFields fails", () => {
        ["test", 1, 2, "1", "2"].forEach((v, i) => {
            assert.commandFailedWithCode(db[kCollName].update({_id: i}, [{$addFields: {$v: i}}]), 16410);
        });
    });

    it("tests that updates using $v inside $replaceWith and $setField and an object literal succeed", () => {
        ["test", 1, 2, "1", "2"].forEach((v, i) => {
            assert.commandWorked(
                db[kCollName].update({_id: i}, [
                    {$replaceWith: {$setField: {field: {$literal: "$v"}, input: "$$ROOT", value: v}}},
                ]),
            );
            let expected = {
                documentKey: {_id: i},
                fullDocument: {_id: i, $v: v},
                ns: {db: "test", coll: kCollName},
                operationType: "replace",
            };
            cst.assertNextChangesEqual({cursor, expectedChanges: [expected]});
        });
    });
});
