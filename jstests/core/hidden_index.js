/**
 * Test expected behavior for hidden indexes. A hidden index is invisible to the query planner so
 * it will not be used in planning. It is handled in the same way as other indexes by the index
 * catalog and for TTL purposes.
 * @tags: [
 *  multiversion_incompatible,
 *  requires_fcv_44,
 *  requires_non_retryable_commands,    // CollMod is not retryable.
 * ]
 */

(function() {
'use strict';
load("jstests/libs/analyze_plan.js");              // For getPlanStages.
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.
load("jstests/libs/get_index_helpers.js");         // For GetIndexHelpers.findByName.

const collName = "hidden_index";
let coll = assertDropAndRecreateCollection(db, collName);

function numOfUsedIXSCAN(query) {
    const explain = assert.commandWorked(coll.find(query).explain());
    const ixScans = getPlanStages(explain.queryPlanner.winningPlan, "IXSCAN");
    return ixScans.length;
}

function validateHiddenIndexBehaviour(query, index_type, wildcard) {
    let index_name;
    if (wildcard)
        index_name = 'a.$**_' + index_type;
    else
        index_name = 'a_' + index_type;

    if (wildcard)
        assert.commandWorked(coll.createIndex({"a.$**": index_type}));
    else
        assert.commandWorked(coll.createIndex({"a": index_type}));

    let idxSpec = GetIndexHelpers.findByName(coll.getIndexes(), index_name);
    assert.eq(idxSpec.hidden, undefined);
    assert.gt(numOfUsedIXSCAN(query), 0);

    assert.commandWorked(coll.hideIndex(index_name));
    idxSpec = GetIndexHelpers.findByName(coll.getIndexes(), index_name);
    assert(idxSpec.hidden);
    if (index_type === "text") {
        assert.commandFailedWithCode(coll.runCommand("find", {filter: query}, {hint: {a: 1}}), 291);
        assert.commandWorked(coll.dropIndexes());
        return;
    }
    assert.eq(numOfUsedIXSCAN(query), 0);

    assert.commandWorked(coll.unhideIndex(index_name));
    idxSpec = GetIndexHelpers.findByName(coll.getIndexes(), index_name);
    assert.eq(idxSpec.hidden, undefined);
    assert.gt(numOfUsedIXSCAN(query), 0);

    assert.commandWorked(coll.dropIndex(index_name));

    if (wildcard)
        assert.commandWorked(coll.createIndex({"a.$**": index_type}, {hidden: true}));
    else
        assert.commandWorked(coll.createIndex({"a": index_type}, {hidden: true}));

    idxSpec = GetIndexHelpers.findByName(coll.getIndexes(), index_name);
    assert(idxSpec.hidden);
    assert.eq(numOfUsedIXSCAN(query), 0);
    assert.commandWorked(coll.dropIndexes());
}

// Normal index testing.
validateHiddenIndexBehaviour({a: 1}, 1);

// GEO index testing.
validateHiddenIndexBehaviour({
    a: {$geoWithin: {$geometry: {type: "Polygon", coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]}}}
},
                             "2dsphere");

// Fts index.
validateHiddenIndexBehaviour({$text: {$search: "java"}}, "text");

// Wildcard index.
validateHiddenIndexBehaviour({"a.f": 1}, 1, true);

// Hidden index on capped collection.
if (!FixtureHelpers.isMongos(db)) {
    coll = assertDropAndRecreateCollection(db, collName, {capped: true, size: 100});
    validateHiddenIndexBehaviour({a: 1}, 1);
    coll = assertDropAndRecreateCollection(db, collName);
}
// Test that index 'hidden' status can be found in listIndexes command.
assert.commandWorked(coll.createIndex({lsIdx: 1}, {hidden: true}));
let res = assert.commandWorked(db.runCommand({"listIndexes": collName}));
let idxSpec = GetIndexHelpers.findByName(res.cursor.firstBatch, "lsIdx_1");
assert.eq(idxSpec.hidden, true);

// Can't hide any index in a system collection.
const systemColl = db.getSiblingDB('admin').system.version;
assert.commandWorked(systemColl.createIndex({a: 1}));
assert.commandFailedWithCode(systemColl.hideIndex("a_1"), 2);
assert.commandFailedWithCode(systemColl.createIndex({a: 1}, {hidden: true}), 2);

// Can't hide the '_id' index.
assert.commandFailed(coll.hideIndex("_id_"));

// Can't 'hint' a hidden index.
assert.commandWorked(coll.createIndex({"a": 1}, {"hidden": true}));
assert.commandFailedWithCode(coll.runCommand("find", {hint: {a: 1}}), 2);

// We can change ttl index and hide info at the same time.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({"tm": 1}, {expireAfterSeconds: 10}));
idxSpec = GetIndexHelpers.findByName(coll.getIndexes(), "tm_1");
assert.eq(idxSpec.hidden, undefined);
assert.eq(idxSpec.expireAfterSeconds, 10);

db.runCommand({
    "collMod": coll.getName(),
    "index": {"name": "tm_1", "expireAfterSeconds": 1, "hidden": true}
});
idxSpec = GetIndexHelpers.findByName(coll.getIndexes(), "tm_1");
assert(idxSpec.hidden);
assert.eq(idxSpec.expireAfterSeconds, 1);
})();
