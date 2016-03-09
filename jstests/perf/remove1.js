/**
 *  Performance tests for removing ojects
 */

var removals = 100;
var size = 500000;
var collection_name = "remove_test";
var msg = "Hello from remove test";

function testSetup(dbConn) {
    var t = dbConn[collection_name];
    t.drop();
    t.ensureIndex({num: 1});

    for (var i = 0; i < size; i++) {
        t.save({num: i, msg: msg});
    }
}

function between(low, high, val, msg) {
    assert(low < val, msg);
    assert(val < high, msg);
}

/**
 *   Compares difference of removing objects from a collection if only includes
 *   field that's indexed, vs w/ additional other fields
 *
 * @param dbConn
 */
function testRemoveWithMultiField(dbConn) {
    var results = {};
    var t = dbConn[collection_name];

    testSetup(dbConn);

    t.remove({num: 0});
    results.indexOnly = Date.timeFunc(function() {
        for (var i = 1; i < removals; i++) {
            t.remove({num: i});
        }

        t.findOne();
    });

    testSetup(dbConn);

    t.remove({num: 0, msg: msg});
    results.withAnother = Date.timeFunc(function() {
        for (var i = 1; i < removals; i++) {
            t.remove({num: i, msg: msg});
        }

        t.findOne();
    });

    between(0.65,
            1.35,
            (results.indexOnly / results.withAnother),
            "indexOnly / withAnother (" + results.indexOnly + " / " + results.withAnother +
                " ) = " + results.indexOnly / results.withAnother + " not in [0.65, 1.35]");
}

testRemoveWithMultiField(db);
