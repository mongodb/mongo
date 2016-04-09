/**
 * Test function roundtripping in documents with --enableJavaScriptProtection.
 *
 * Ensure that:
 * 1. A function stored in a document can be loaded into a Code()
 *    object in the mongo shell with the --enableJavaScriptProtection flag.
 * 2. A Code object is correctly serialized as BSON type 'Code' or
 *    'CodeWScope'.
 */
(function() {
    "use strict";

    var testServer = MongoRunner.runMongod({setParameter: "javascriptProtection=true"});
    assert.neq(
        null, testServer, "failed to start mongod with --setParameter=javascriptProtection=true");

    var db = testServer.getDB("test");
    var t = db.js_protection_roundtrip;

    function makeRoundTrip() {
        var functionToEval = function() {
            var doc = db.js_protection_roundtrip.findOne({_id: 0});
            assert.neq(null, doc);
            db.js_protection_roundtrip.insertOne({_id: 1, myFunc: doc.myFunc});
        };

        var exitCode = runMongoProgram("mongo",
                                       "--port",
                                       testServer.port,
                                       "--enableJavaScriptProtection",
                                       "--eval",
                                       "(" + functionToEval.toString() + ")();");
        assert.eq(0, exitCode);
    }

    /**
     *  ACTUAL TEST
     */

    t.insertOne({
        _id: 0,
        myFunc: function() {
            return "yes";
        }
    });

    makeRoundTrip();

    var doc = t.findOne({_id: 1});
    assert.neq(null, doc, "failed to find document inserted by other mongo shell");

    assert(doc.hasOwnProperty("myFunc"), tojson(doc));
    assert.eq("function", typeof doc.myFunc, tojson(doc));
    assert.eq("yes", doc.myFunc(), tojson(doc));

    MongoRunner.stopMongod(testServer);
})();
