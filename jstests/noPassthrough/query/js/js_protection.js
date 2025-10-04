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
 *
 * @tags: [
 *   requires_scripting
 * ]
 */

let testServer = MongoRunner.runMongod({setParameter: "javascriptProtection=true"});
assert.neq(null, testServer, "failed to start mongod with --setParameter=javascriptProtection=true");

var db = testServer.getDB("test");
let t = db.js_protection;

function assertMongoClientCorrect() {
    /* eslint-disable */
    var functionToEval = function () {
        var doc = db.js_protection.findOne({_id: 0});
        assert.neq(null, doc);
        assert(doc.hasOwnProperty("myFunc"));
        assert.neq("function", typeof doc.myFunc, "value of BSON type Code shouldn't have been eval()ed automatically");

        assert.eq("undefined", typeof addOne, "addOne function has already been defined");
        db.loadServerScripts();
        assert.neq("undefined", typeof addOne, "addOne function should have been eval()ed locally");
        assert.eq(5, addOne(4));
    };
    /* eslint-enable */

    let exitCode = runMongoProgram(
        "mongo",
        "--port",
        testServer.port,
        "--enableJavaScriptProtection",
        "--eval",
        "(" + functionToEval.toString() + ")();",
    );
    assert.eq(0, exitCode);
}

function assertNoStoredWhere() {
    t.insertOne({name: "testdoc", val: 0, y: 0});

    let res = t.update({$where: "addOne(this.val) === 1"}, {$set: {y: 100}}, false, true);
    assert.writeError(res);

    let doc = t.findOne({name: "testdoc"});
    assert.neq(null, doc);
    assert.eq(0, doc.y, tojson(doc));

    res = t.update(
        {
            $where: function () {
                return this.val === 0;
            },
        },
        {$set: {y: 100}},
        false,
        true,
    );
    assert.commandWorked(res);

    doc = t.findOne({name: "testdoc"});
    assert.neq(null, doc);
    assert.eq(100, doc.y, tojson(doc));
}

/**
 *  ACTUAL TEST
 */

db.system.js.insertOne({
    _id: "addOne",
    value: function (x) {
        return x + 1;
    },
});

t.insertOne({
    _id: 0,
    myFunc: function () {
        return "testval";
    },
});

assertMongoClientCorrect();
assertNoStoredWhere();

MongoRunner.stopMongod(testServer);
