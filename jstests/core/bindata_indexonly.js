/**
 * This test ensures that range predicates with a BinData value:
 * 1) Return the correct documents.
 * 2) Can perform index-only data access.
 */
(function() {
    'use strict';

    load("jstests/libs/analyze_plan.js");

    var coll = db.jstests_bindata_indexonly;

    coll.drop();
    assert.writeOK(coll.insert({_id: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"), a: 1}));
    assert.writeOK(coll.insert({_id: BinData(0, "AQAAAAEBAAVlbl9VSwAAAAAAAAhv"), a: 2}));
    assert.writeOK(coll.insert({_id: BinData(0, "AQAAAAEBAAVlbl9VSwAAAAAAAAhz"), a: 3}));
    assert.writeOK(coll.insert({_id: BinData(0, "////////////////////////////"), a: 4}));
    assert.commandWorked(coll.createIndex({_id: 1, a: 1}));

    function testIndexOnlyBinData(blob) {
        var explain =
            coll.find({$and: [{_id: {$lte: BinData(0, blob)}}, {_id: {$gte: BinData(0, blob)}}]},
                      {_id: 1, a: 1})
                .hint({_id: 1, a: 1})
                .explain("executionStats");

        assert(isIndexOnly(explain.queryPlanner.winningPlan),
               "indexonly.BinData(0, " + blob + ") - must be index-only");
        assert.eq(1,
                  explain.executionStats.nReturned,
                  "EXACTone.BinData(0, " + blob + ") - should only return one in unique set");
    }

    testIndexOnlyBinData("AAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    testIndexOnlyBinData("AQAAAAEBAAVlbl9VSwAAAAAAAAhv");
    testIndexOnlyBinData("AQAAAAEBAAVlbl9VSwAAAAAAAAhz");
    testIndexOnlyBinData("////////////////////////////");

    var explain;

    explain = coll.find({_id: {$lt: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")}}, {_id: 1, a: 1})
                  .hint({_id: 1, a: 1})
                  .explain("executionStats");
    assert(isIndexOnly(explain), "indexonly.$lt.1 - must be index-only");
    assert.eq(0,
              explain.executionStats.nReturned,
              "correctcount.$lt.1 - not returning correct documents");

    explain = coll.find({_id: {$gt: BinData(0, "////////////////////////////")}}, {_id: 1, a: 1})
                  .hint({_id: 1, a: 1})
                  .explain("executionStats");
    assert(isIndexOnly(explain), "indexonly.$gt.2 - must be index-only");
    assert.eq(0,
              explain.executionStats.nReturned,
              "correctcount.$gt.2 - not returning correct documents");

    explain = coll.find({_id: {$lte: BinData(0, "AQAAAAEBAAVlbl9VSwAAAAAAAAhv")}}, {_id: 1, a: 1})
                  .hint({_id: 1, a: 1})
                  .explain("executionStats");
    assert(isIndexOnly(explain), "indexonly.$lte.3 - must be index-only");
    assert.eq(2,
              explain.executionStats.nReturned,
              "correctcount.$lte.3 - not returning correct documents");

    explain = coll.find({_id: {$gte: BinData(0, "AQAAAAEBAAVlbl9VSwAAAAAAAAhz")}}, {_id: 1, a: 1})
                  .hint({_id: 1, a: 1})
                  .explain("executionStats");
    assert(isIndexOnly(explain), "indexonly.$gte.3 - must be index-only");
    assert.eq(2,
              explain.executionStats.nReturned,
              "correctcount.$gte.3 - not returning correct documents");

    coll.drop();
})();