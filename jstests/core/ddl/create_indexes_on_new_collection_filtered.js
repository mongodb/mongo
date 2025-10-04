/**
 * Test implicitly creating a collection via createIndex() with the id index explicitly specified,
 * which is the only way to hit the code path where an index already exists when implicitly creating
 * a collection.
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const testDB = db.getSiblingDB(jsTestName());

let t = testDB.id_only;
t.drop();
assert.commandWorked(t.createIndexes([{_id: 1}]));
assert.eq(t.getIndexes().length, 1 + FixtureHelpers.isSharded(t));

t = testDB.id_and_secondary_indexes;
t.drop();
assert.commandWorked(t.createIndexes([{a: 1}, {b: 1}, {_id: 1}]));
assert.eq(t.getIndexes().length, 3 + FixtureHelpers.isSharded(t));
