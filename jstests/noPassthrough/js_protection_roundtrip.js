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

    var testServer = MongoRunner.runMongod();
    assert.neq(null, testServer, "failed to start mongod");
    var db = testServer.getDB("test");
    var t = db.js_protection_roundtrip;

    function withoutJavaScriptProtection() {
        var doc = db.js_protection_roundtrip.findOne({_id: 0});
        assert.neq(doc, null);
        assert.eq(typeof doc.myFunc, "function", "myFunc should have been presented as a function");
        assert.eq(doc.myFunc(), "yes");
    }

    function withJavaScriptProtection() {
        var doc = db.js_protection_roundtrip.findOne({_id: 0});
        assert.neq(doc, null);
        assert(doc.myFunc instanceof Code, "myFunc should have been a Code object");
        doc.myFunc = eval("(" + doc.myFunc.code + ")");
        assert.eq(doc.myFunc(), "yes");
    }

    function testFunctionUnmarshall(jsProtection, evalFunc) {
        var evalString = "(" + tojson(evalFunc) + ")();";
        var protectionFlag =
            jsProtection ? "--enableJavaScriptProtection" : "--disableJavaScriptProtection";
        var exitCode = runMongoProgram(
            "mongo", "--port", testServer.port, protectionFlag, "--eval", evalString);
        assert.eq(exitCode, 0);
    }

    /**
     *  ACTUAL TEST
     */
    var result = t.insert({
        _id: 0,
        myFunc: function() {
            return "yes";
        }
    });
    assert.writeOK(result);

    testFunctionUnmarshall(true, withJavaScriptProtection);
    testFunctionUnmarshall(false, withoutJavaScriptProtection);

    MongoRunner.stopMongod(testServer);
})();
