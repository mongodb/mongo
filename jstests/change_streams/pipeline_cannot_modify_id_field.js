/**
 * Tests that stages which modify or remove the _id field are not permitted to run in a
 * $changeStream pipeline.
 */
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    const coll = assertDropAndRecreateCollection(db, jsTestName());

    // Bare-bones $changeStream pipeline which will be augmented during tests.
    const changeStream = [{$changeStream: {}}];

    // Test-cases of transformations that modify or remove _id, and are thus disallowed.
    const idModifyingTransformations = [
        {$project: {_id: 0}},
        {$project: {_id: "newValue"}},
        {$project: {_id: "$otherField"}},
        {$project: {_id: 0, otherField: 0}},
        {$project: {_id: 0, otherField: 1}},
        {$project: {"_id._data": 0}},
        {$project: {"_id._data": 1}},
        {$project: {"_id._data": "newValue"}},
        {$project: {"_id._data": "$_id._data"}},  // Disallowed because it discards _typeBits.
        {$project: {"_id._data": "$otherField"}},
        {$project: {"_id.otherField": 1}},
        {$project: {"_id._typeBits": 0}},
        [
          {$project: {otherField: "$_id"}},
          {$project: {otherField: 0}},
          {$project: {_id: "$otherField"}}
        ],
        {$project: {_id: {data: "$_id._data", typeBits: "$_id._typeBits"}}},    // Fields renamed.
        {$project: {_id: {_typeBits: "$_id._typeBits", _data: "$_id._data"}}},  // Fields reordered.
        {$project: {_id: {_data: "$_id._typeBits", _typeBits: "$_id._data"}}},  // Fields swapped.
        {$addFields: {_id: "newValue"}},
        {$addFields: {_id: "$otherField"}},
        {$addFields: {"_id._data": "newValue"}},
        {$addFields: {"_id._data": "$otherField"}},
        {$addFields: {"_id.otherField": "newValue"}},  // New subfield added to _id.
        [
          {$addFields: {otherField: "$_id"}},
          {$addFields: {otherField: "newValue"}},
          {$addFields: {_id: "$otherField"}}
        ],
        [
          // Fields renamed.
          {$addFields: {newId: {data: "$_id._data", typeBits: "$_id._typeBits"}}},
          {$addFields: {_id: "$newId"}}
        ],
        [
          // Fields reordered.
          {$addFields: {newId: {_typeBits: "$_id._typeBits", _data: "$_id._data"}}},
          {$addFields: {_id: "$newId"}}
        ],
        [
          // Fields swapped.
          {$addFields: {newId: {_data: "$_id._typeBits", _typeBits: "$_id._data"}}},
          {$addFields: {_id: "$newId"}}
        ],
        {$replaceRoot: {newRoot: {otherField: "$_id"}}},
        {$redact: {$cond: {if: {$gt: ["$_id", {}]}, then: "$$DESCEND", else: "$$PRUNE"}}}  // _id:0
    ];

    // Test-cases of projections which are allowed: explicit inclusion of _id, implicit inclusion of
    // _id, renames which retain the full _id field, exclusion of unrelated fields, addition of and
    // modifications to unrelated fields, sequential renames which ultimately preserve _id, etc.
    const idPreservingTransformations = [
        {$project: {_id: 1}},
        {$project: {_id: 1, otherField: 0}},
        {$project: {_id: 1, otherField: 1}},
        {$project: {_id: "$_id", otherField: 1}},
        {$project: {"_id.otherField": 0}},
        {$project: {otherField: 1}},
        {$project: {otherField: 0}},
        {$project: {otherField: "$_id"}},
        [
          {$project: {otherField: "$_id"}},
          {$project: {otherField: 1}},
          {$project: {_id: "$otherField"}}
        ],
        {$project: {"_id._data": 1, "_id._typeBits": 1}},
        {$project: {_id: {_data: "$_id._data", _typeBits: "$_id._typeBits"}}},
        {$addFields: {_id: "$_id"}},
        {$addFields: {otherField: "newValue"}},
        {$addFields: {_id: {_data: "$_id._data", _typeBits: "$_id._typeBits"}}},
        [{$addFields: {otherField: "$_id"}}, {$addFields: {_id: "$otherField"}}],
        [
          {$addFields: {newId: {_data: "$_id._data", _typeBits: "$_id._typeBits"}}},
          {$addFields: {_id: "$newId"}}
        ],
        {$replaceRoot: {newRoot: {_id: "$_id"}}},
        {
          $redact: {
              $cond: {
                  if: {
                      $or: [
                          // Keeps _id, descends into fullDocument.
                          {$not: {$isArray: "$tags"}},
                          {$gt: [{$size: {$setIntersection: ["$tags", ["USA"]]}}, 0]}
                      ]
                  },
                  then: "$$DESCEND",
                  else: "$$PRUNE"
              }
          }
        },
        {$redact: "$$DESCEND"},  // Descends through entire document, retaining all of it.
        {$redact: "$$KEEP"}      // Keeps entire document.
    ];

    let docId = 0;

    // Verify that each of the whitelisted transformations above succeeds.
    for (let transform of idPreservingTransformations) {
        const cmdRes = assert.commandWorked(
            db.runCommand(
                {aggregate: coll.getName(), pipeline: changeStream.concat(transform), cursor: {}}),
            transform);
        assert.commandWorked(coll.insert({_id: docId++}));
        assert.soon(() => {
            const getMoreRes = assert.commandWorked(
                db.runCommand({getMore: cmdRes.cursor.id, collection: coll.getName()}), transform);
            return getMoreRes.cursor.nextBatch.length > 0;
        }, transform);
    }

    // Verify that each of the blacklisted transformations above are rejected.
    for (let transform of idModifyingTransformations) {
        const cmdRes = assert.commandWorked(
            db.runCommand(
                {aggregate: coll.getName(), pipeline: changeStream.concat(transform), cursor: {}}),
            transform);
        assert.commandWorked(coll.insert({_id: docId++}));
        assert.soon(() => {
            const getMoreRes =
                db.runCommand({getMore: cmdRes.cursor.id, collection: coll.getName()});
            return !getMoreRes.ok &&
                assert.commandFailedWithCode(getMoreRes, [51059, 51060], transform);
        }, transform);
    }
}());