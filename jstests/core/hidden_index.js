/**
 * Test expected behavior for hidden indexes. A hidden index is invisible to the query planner so
 * it will not be used in planning. It is handled in the same way as other indexes by the index
 * catalog and for TTL purposes.
 * @tags: [
 *   # CollMod is not retryable.
 *   requires_non_retryable_commands,
 * ]
 */

(function() {
'use strict';
load("jstests/libs/analyze_plan.js");              // For getPlanStages.
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.
load("jstests/libs/index_catalog_helpers.js");     // For IndexCatalogHelpers.findByName.
load("jstests/libs/sbe_util.js");                  // For checkSBEEnabled.

const columnstoreEnabled =
    checkSBEEnabled(db, ["featureFlagColumnstoreIndexes", "featureFlagSbeFull"]);

const collName = "hidden_index";
let coll = assertDropAndRecreateCollection(db, collName);

function numOfUsedIndexes(query, projection) {
    const explain = assert.commandWorked(coll.find(query, projection).explain());
    const indexScans = getPlanStages(getWinningPlan(explain.queryPlanner), "IXSCAN");
    const columnIndexScans = getPlanStages(getWinningPlan(explain.queryPlanner), "COLUMN_SCAN");
    return indexScans.length + columnIndexScans.length;
}

function validateHiddenIndexBehaviour(
    {query = {}, projection = {}, index_type = 1, wildcard = false}) {
    let index_name;
    if (wildcard)
        index_name = "a.$**_" + index_type;
    else if (index_type === 'columnstore')
        index_name = "$**_" + index_type;
    else
        index_name = "a_" + index_type;

    if (wildcard)
        assert.commandWorked(coll.createIndex({"a.$**": index_type}));
    else if (index_type === 'columnstore')
        assert.commandWorked(coll.createIndex({"$**": index_type}));
    else
        assert.commandWorked(coll.createIndex({"a": index_type}));

    let idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), index_name);
    assert.eq(idxSpec.hidden, undefined);
    assert.gt(numOfUsedIndexes(query, projection), 0);

    assert.commandWorked(coll.hideIndex(index_name));
    idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), index_name);
    assert(idxSpec.hidden);
    if (index_type === "text") {
        assert.commandFailedWithCode(coll.runCommand("find", {filter: query}, {hint: {a: 1}}), 291);
        assert.commandWorked(coll.dropIndexes());
        return;
    }
    assert.eq(numOfUsedIndexes(query, projection), 0);

    assert.commandWorked(coll.unhideIndex(index_name));
    idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), index_name);
    assert.eq(idxSpec.hidden, undefined);
    assert.gt(numOfUsedIndexes(query, projection), 0);

    assert.commandWorked(coll.dropIndex(index_name));

    if (wildcard)
        assert.commandWorked(coll.createIndex({"a.$**": index_type}, {hidden: true}));
    else if (index_type === 'columnstore')
        assert.commandWorked(coll.createIndex({"$**": index_type}, {hidden: true}));
    else
        assert.commandWorked(coll.createIndex({"a": index_type}, {hidden: true}));

    idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), index_name);
    assert(idxSpec.hidden);
    assert.eq(numOfUsedIndexes(query, projection), 0);
    assert.commandWorked(coll.dropIndexes());
}

// Normal index testing.
validateHiddenIndexBehaviour({query: {a: 1}, index_type: 1});

// GEO index testing.
validateHiddenIndexBehaviour({
    query: {
        a: {
            $geoWithin:
                {$geometry: {type: "Polygon", coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]}}
        }
    },
    index_type: "2dsphere"
});

// Fts index.
validateHiddenIndexBehaviour({query: {$text: {$search: "java"}}, index_type: "text"});

// Wildcard index.
validateHiddenIndexBehaviour({query: {"a.f": 1}, index_type: 1, wildcard: true});

// Column Store Index.
if (columnstoreEnabled) {
    validateHiddenIndexBehaviour({projection: {a: 1}, index_type: "columnstore"});
} else {
    jsTestLog(
        "Skipping columnstore hidden index validation test since the feature flag is not enabled.");
}

// Hidden index on capped collection.
if (!FixtureHelpers.isMongos(db)) {
    coll = assertDropAndRecreateCollection(db, collName, {capped: true, size: 100});
    validateHiddenIndexBehaviour({query: {a: 1}, index_type: 1});
    coll = assertDropAndRecreateCollection(db, collName);
}
// Test that index 'hidden' status can be found in listIndexes command.
assert.commandWorked(coll.createIndex({lsIdx: 1}, {hidden: true}));
let res = assert.commandWorked(db.runCommand({"listIndexes": collName}));
let idxSpec = IndexCatalogHelpers.findByName(res.cursor.firstBatch, "lsIdx_1");
assert.eq(idxSpec.hidden, true);

// Can't hide any index in a system collection.
const systemColl = db.getSiblingDB("admin").system.version;
assert.commandWorked(systemColl.createIndex({a: 1}));
// The collMod command throws NoShardingEnabled on DDL coordinator implementation and BadValue on
// old implementation.
assert.commandFailedWithCode(systemColl.hideIndex("a_1"),
                             [ErrorCodes.NoShardingEnabled, ErrorCodes.BadValue]);
assert.commandFailedWithCode(systemColl.createIndex({a: 1}, {hidden: true}), 2);

// Can't hide the '_id' index.
assert.commandFailed(coll.hideIndex("_id_"));

// Can't 'hint' a hidden index.
assert.commandWorked(coll.createIndex({"a": 1}, {"hidden": true}));
assert.commandFailedWithCode(coll.runCommand("find", {hint: {a: 1}}), 2);

// We can change ttl index and hide info at the same time.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({"tm": 1}, {expireAfterSeconds: 10}));
idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), "tm_1");
assert.eq(idxSpec.hidden, undefined);
assert.eq(idxSpec.expireAfterSeconds, 10);

db.runCommand({
    "collMod": coll.getName(),
    "index": {"name": "tm_1", "expireAfterSeconds": 1, "hidden": true}
});
idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), "tm_1");
assert(idxSpec.hidden);
assert.eq(idxSpec.expireAfterSeconds, 1);

//
// Ensure that "hidden: false" won't be added to index specification.
//
assert.commandWorked(
    db.runCommand({createIndexes: collName, indexes: [{key: {y: 1}, name: "y", hidden: false}]}));
idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), "y");
assert.eq(idxSpec.hidden, undefined);

assert.commandWorked(coll.hideIndex("y"));
idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), "y");
assert(idxSpec.hidden);

// Ensure that unhiding the hidden index won't add 'hidden: false' to the index spec as well.
assert.commandWorked(coll.unhideIndex("y"));
idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), "y");
assert.eq(idxSpec.hidden, undefined);
})();
