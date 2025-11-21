/**
 * Verify that distinct works correctly when setting a collator (SERVER-100108).
 * @tags: [
 *   assumes_read_concern_unchanged,
 *   assumes_write_concern_unchanged,
 *   does_not_support_transactions,
 *   does_not_support_stepdowns,
 *   # Known unclear issues with distinct on timeseries.
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
let coll = assertDropAndRecreateCollection(db, jsTestName());

let docs = [];
docs.push({_id: {a: []}});
docs.push({_id: {a: [2]}});
docs.push({_id: {a: [2, 3]}});
docs.push({_id: {a: [2, 3]}});
docs.push({_id: 5});
coll.insert(docs);

const locales = ["bn@collation=traditional", "de@collation=phonebook", "ca", "fr_CA", "simple"];

locales.forEach((locale) => {
    jsTest.log(`Testing locale ${locale}...`);

    // Test distinct command.
    {
        const res = coll.runCommand("distinct", {
            key: "_id.a.0",
            query: {_id: {$in: [{a: [2, 3]}, 5]}},
            collation: {locale},
        });
        assert.commandWorked(res);
        assert.eq([2], res.values);
    }

    // Test find command.
    {
        const res = db.runCommand({
            find: coll.getName(),
            filter: {_id: {$in: [{a: [2, 3]}, 5]}},
            collation: {locale},
        });
        assert.commandWorked(res);
        assert.eq([{_id: {a: [2, 3]}}], res.cursor.firstBatch);
    }

    // Test aggregate command.
    {
        const res = db.runCommand({
            aggregate: coll.getName(),
            pipeline: [{$match: {_id: {$in: [{a: [2, 3]}, 5]}}}],
            collation: {locale},
            cursor: {},
        });
        assert.commandWorked(res);
        assert.eq([{_id: {a: [2, 3]}}], res.cursor.firstBatch);
    }
});

// Verify that $sortArray can be used in distinct command.
coll = assertDropAndRecreateCollection(db, jsTestName());
coll.insert({array: ["yellow", "red"]});
coll.insert({array: ["red", "yellow"]});
assert.commandWorked(
    db.runCommand({
        distinct: jsTestName(),
        key: "array",
        query: {$expr: {$sortArray: {input: "$array", sortBy: -1}}},
        collation: {locale: "el"},
    }),
);
