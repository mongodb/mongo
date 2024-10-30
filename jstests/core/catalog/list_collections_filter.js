/*
 * Test SERVER-18622 listCollections should special case filtering by name.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: applyOps.
 *   not_allowed_with_signed_security_token,
 *   # applyOps is not supported on mongos
 *   assumes_against_mongod_not_mongos,
 *   # Requires no extra options present
 *   incompatible_with_preimages_by_default,
 *   # TODO (SERVER-89668): Remove tag. Currently incompatible due to collection
 *   # options containing the recordIdsReplicated:true option, which
 *   # this test dislikes.
 *   exclude_when_record_ids_replicated
 * ]
 */

import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";

const mydb = db.getSiblingDB("list_collections_filter");
assert.commandWorked(mydb.dropDatabase());

// If the test fixture is enabling clustered collections by default, all test collections created
// in this test will have a 'clusteredIndex' field in the collection options returned by
// listCollections.
const defaultCollectionOptionsFilter =
    ClusteredCollectionUtil.areAllCollectionsClustered(mydb.getMongo())
    ? {'options.clusteredIndex.unique': true}
    : {options: {}};

// Make some collections.
assert.commandWorked(mydb.createCollection("lists"));
assert.commandWorked(mydb.createCollection("ordered_sets"));
assert.commandWorked(mydb.createCollection("unordered_sets"));
assert.commandWorked(mydb.runCommand(
    {applyOps: [{op: "c", ns: mydb.getName() + ".$cmd", o: {create: "arrays_temp", temp: true}}]}));

/**
 * Asserts that the names of the collections returned from running the listCollections
 * command with the given filter match the expected names.
 */
function testListCollections(filter, expectedNames) {
    if (filter === undefined) {
        filter = {};
    }

    const cursor = new DBCommandCursor(mydb, mydb.runCommand("listCollections", {filter: filter}));
    function stripToName(result) {
        return result.name;
    }

    // Sometimes the system.profile collection gets created due to a slow machine. We exclude it
    // from the resulting names.
    function isNotProfileCollection(name) {
        return name !== "system.profile";
    }

    const cursorResultNames = cursor.toArray().map(stripToName).filter(isNotProfileCollection);

    assert.eq(cursorResultNames.sort(), expectedNames.sort());

    // Assert the shell helper returns the same list, but in sorted order.
    const shellResultNames =
        mydb.getCollectionInfos(filter).map(stripToName).filter(isNotProfileCollection);
    assert.eq(shellResultNames, expectedNames.sort());
}

// No filter.
testListCollections({}, ["lists", "ordered_sets", "unordered_sets", "arrays_temp"]);

// Filter without name.
testListCollections(defaultCollectionOptionsFilter, ["lists", "ordered_sets", "unordered_sets"]);

// Filter with exact match on name.
testListCollections({name: "lists"}, ["lists"]);
testListCollections({name: "non-existent"}, []);
testListCollections({name: ""}, []);
testListCollections({name: 1234}, []);

// Filter with $in.
testListCollections({name: {$in: ["lists"]}}, ["lists"]);
testListCollections({name: {$in: []}}, []);
testListCollections({name: {$in: ["lists", "ordered_sets", "non-existent", "", 1234]}},
                    ["lists", "ordered_sets"]);
// With a regex.
testListCollections({name: {$in: ["lists", /.*_sets$/, "non-existent", "", 1234]}},
                    ["lists", "ordered_sets", "unordered_sets"]);

// Filter with $and.
testListCollections(Object.merge({name: "lists"}, defaultCollectionOptionsFilter), ["lists"]);
testListCollections({name: "lists", 'options.temp': true}, []);
testListCollections({$and: [{name: "lists"}, {'options.temp': true}]}, []);
testListCollections({name: "arrays_temp", 'options.temp': true}, ["arrays_temp"]);

// Filter with $and and $in.
testListCollections(
    Object.merge({name: {$in: ["lists", /.*_sets$/]}}, defaultCollectionOptionsFilter),
    ["lists", "ordered_sets", "unordered_sets"]);
testListCollections({
    $and: [
        {name: {$in: ["lists", /.*_sets$/]}},
        {name: "lists"},
        defaultCollectionOptionsFilter,
    ]
},
                    ["lists"]);
testListCollections({
    $and: [
        {name: {$in: ["lists", /.*_sets$/]}},
        {name: "non-existent"},
        defaultCollectionOptionsFilter,
    ]
},
                    []);

// Filter with $expr.
testListCollections({$expr: {$eq: ["$name", "lists"]}}, ["lists"]);

// Filter with $expr with an unbound variable.
assert.throws(function() {
    mydb.getCollectionInfos({$expr: {$eq: ["$name", "$$unbound"]}});
});

// Filter with $expr with a runtime error.
assert.throws(function() {
    mydb.getCollectionInfos({$expr: {$abs: "$name"}});
});

// No extensions are allowed in filters.
assert.throws(function() {
    mydb.getCollectionInfos({$text: {$search: "str"}});
});
assert.throws(function() {
    mydb.getCollectionInfos({
        $where: function() {
            return true;
        }
    });
});
assert.throws(function() {
    mydb.getCollectionInfos({a: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}});
});
