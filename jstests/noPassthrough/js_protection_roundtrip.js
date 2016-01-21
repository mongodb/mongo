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

var testServer = MongoRunner.runMongod({setParameter: 'javascriptProtection=true'}),
    db = testServer.getDB("test"),
    t = db.foo,
    x;

function makeRoundTrip() {
    var mongo = runMongoProgram("mongo",
                                "--port", testServer.port,
                                "--enableJavaScriptProtection",
                                "--eval",
        "var x = db.foo.findOne({'_id' : 0});" +
        "db.foo.insertOne({'_id': 1, myFunc: x.myFunc});" +
        "print(\"completed gracefully\");"
    );

    var mongoOutput = rawMongoProgramOutput();
    assert(!mongoOutput.match(/assert failed/));
    assert(mongoOutput.match(/completed gracefully/));
}

/**
 *  ACTUAL TEST
 */

t.insertOne({'_id': 0, 'myFunc': function() { return 'yes'; } });

makeRoundTrip();

x = t.findOne({'_id': 1});

if (!x.myFunc() == 'yes') {
    assert(0);
}

MongoRunner.stopMongod(testServer);
})();
