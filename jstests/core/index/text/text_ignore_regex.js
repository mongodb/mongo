/**
 * Tests that regexes aren't allowed in text indexes.
 *
 * @tags: [
 * ]
 */

import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

// Set collection and drop.
const collName = jsTestName();
const coll = db[collName];
assertDropCollection(db, collName);

// Insert data.
assert.commandWorked(coll.insert({z: "hello", a: "hello"}));
// Create text index.
assert.commandWorked(coll.createIndex({z: 1, a: "text"}));

// Ensure equality predicates in the prefix still work.
assert.commandWorked(coll.find({$text: {$search: "hello"}, z: "hello"}).explain("executionStats"));

// Ensure no solutions were found for regexes in the prefix.
assert.throwsWithCode(() => coll.find({$text: {$search: "hello"}, z: /hello/}).toArray(),
                      [ErrorCodes.NoQueryExecutionPlans]);

assertDropCollection(db, collName);
