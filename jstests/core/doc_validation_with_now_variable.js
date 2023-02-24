/**
 * Tests that insertion with a $$NOW Validator works properly.
 *
 * @tags: [assumes_no_implicit_collection_creation_after_drop]
 */
(function() {
"use strict";
const coll = db.coll_doc_validation_with_now_variable;
coll.drop();
// assert.commandWorked(db.createCollection("coll_doc_validation_with_now_variable",
//                                          {validator: {"$expr": {$gt: ["$ts", "$$NOW"]}}}));

assert.commandWorked(db.createCollection("coll_doc_validation_with_now_variable",
                                         {validator: {"$expr": {$lt: ["$ts", "$$NOW"]}}}));

assert.commandWorked(coll.insert({"ts": new Date(1589617694938)}));

const result = coll.insert({"ts": new Date(2708791380000)});
assert.commandFailedWithCode(result, ErrorCodes.DocumentValidationFailure, tojson(result));
})();
