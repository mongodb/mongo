// Tests basic functionality of read-only, non-materialized views.

(function() {
    "use strict";

    // For arrayEq.
    load("jstests/aggregation/extras/utils.js");

    let viewsDB = db.getSiblingDB("views_basic");
    assert.commandWorked(viewsDB.dropDatabase());

    let assertCmdResultEq = function(cmd, expected) {
        let res = viewsDB.runCommand(cmd);
        assert.commandWorked(res);

        let cursor = new DBCommandCursor(db.getMongo(), res, 5);
        let actual = cursor.toArray();
        assert(arrayEq(actual, expected),
               "actual: " + tojson(cursor.toArray()) + ", expected:" + tojson(expected));
    };

    // Insert some control documents.
    let coll = viewsDB.getCollection("collection");
    let bulk = coll.initializeUnorderedBulkOp();
    bulk.insert({_id: "New York", state: "NY", pop: 7});
    bulk.insert({_id: "Oakland", state: "CA", pop: 3});
    bulk.insert({_id: "Palo Alto", state: "CA", pop: 10});
    bulk.insert({_id: "San Francisco", state: "CA", pop: 4});
    bulk.insert({_id: "Trenton", state: "NJ", pop: 5});
    assert.writeOK(bulk.execute());

    // Test creating views on both collections and other views, using the database command and the
    // shell helper.
    assert.commandWorked(viewsDB.runCommand(
        {create: "californiaCities", viewOn: "collection", pipeline: [{$match: {state: "CA"}}]}));
    assert.commandWorked(viewsDB.createView("largeCaliforniaCities",
                                            "californiaCities",
                                            [{$match: {pop: {$gte: 10}}}, {$sort: {pop: 1}}]));

    // Use the find command on a view with various options.
    assertCmdResultEq(
        {find: "californiaCities", filter: {}, projection: {_id: 1, pop: 1}},
        [{_id: "Oakland", pop: 3}, {_id: "Palo Alto", pop: 10}, {_id: "San Francisco", pop: 4}]);
    assertCmdResultEq({find: "largeCaliforniaCities", filter: {pop: {$lt: 50}}, limit: 1},
                      [{_id: "Palo Alto", state: "CA", pop: 10}]);

    // Use aggregation on a view.
    assertCmdResultEq({
        aggregate: "californiaCities",
        pipeline: [{$group: {_id: "$state", totalPop: {$sum: "$pop"}}}],
        cursor: {}
    },
                      [{_id: "CA", totalPop: 17}]);
}());
