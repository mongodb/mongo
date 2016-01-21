/**
 * Positive tests for the behavior of --enableJavaScriptProtection (the flag).
 *
 * Ensure that:
 * 1. Simple functions stored in documents are not automatically marshalled
 *    when the flag is on in the shell.
 * 2. $where is unable to use stored functions when the flag is set on the
 *    server.
 * 3. db.loadServerScripts performs as expected even with the flag is set in
 *    the shell.
 */

(function() {
"use strict";

var testServer = MongoRunner.runMongod({setParameter: 'javascriptProtection=true'});
var db = testServer.getDB("test");
var t = db.foo;
var funcToStore = function(x) { return x + 1; };

function assertMongoClientCorrect() {
    var mongo = runMongoProgram("mongo",
                                "--port", testServer.port,
                                "--enableJavaScriptProtection",
                                "--eval",
    // stored functions in objects
        "var x = db.foo.findOne({'_id' : 0});" +
        "assert.neq(typeof x.foo, 'function');" +
    // retain db.loadServerScripts() functionality
        "db.loadServerScripts();" +
        "assert.eq(stored_func(4), 5);" +

        "print(\"completed gracefully\");"
    );

    var mongoOutput = rawMongoProgramOutput();
    assert(!mongoOutput.match(/assert failed/));
    assert(mongoOutput.match(/completed gracefully/));
}

function assertNoStoredWhere() {
    t.insertOne({name: 'testdoc', val : 0, y : 0});
    t.update( { $where : "stored_func(this.val) == 1" },
              { $set : { y : 100 } } , false , true );

    var x = t.findOne({name: 'testdoc'});
    assert.eq(x.y, 0);

    t.update( { $where : function() { return this.val == 0;} } ,
              { $set : { y : 100 } } , false , true );

    x = t.findOne({name: 'testdoc'});
    assert.eq(x.y, 100);
}

/**
 *  ACTUAL TEST
 */

db.system.js.save( { _id : "stored_func" , value : funcToStore } )
t.insertOne({'_id': 0, 'myFunc': function() { return 'tesval'; } });

assertMongoClientCorrect();
assertNoStoredWhere();

MongoRunner.stopMongod(testServer);
})();
