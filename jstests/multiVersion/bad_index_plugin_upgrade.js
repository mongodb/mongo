assert.matches = function(regex, thing, msg){
    if (assert._debug && msg) print("in assert for: " + msg);

    var str = (typeof(thing) == 'string' ? thing : tojson(thing));
    if (!regex.test(str))
        oassert("regex " + tojson(regex) + " doesn't match '" + str + "' : " + msg);
}

assert.noMatches = function(regex, thing, msg){
    if (assert._debug && msg) print("in assert for: " + msg);

    var str = (typeof(thing) == 'string' ? thing : tojson(thing));
    if (regex.test(str))
        doassert("regex " + tojson(regex) + " matches '" + str + "' : " + msg);
}


var oldVersion = "2.2";
var newVersion = "latest";
var dbName = "index_plugin_upgrade";
var collName = "collection";
var ns = dbName + '.' + collName;

function restartWithNew(conn) {
    MongoRunner.stopMongod(conn);
    return MongoRunner.runMongod({restart: conn, remember: true, binVersion: newVersion});
}

function restartWithOld(conn) {
    MongoRunner.stopMongod(conn);
    return MongoRunner.runMongod({restart: conn, remember: true, binVersion: oldVersion});
}

// Build a real "2d", a fake "2dsphere", and a gibberish index with 2.2
var conn = MongoRunner.runMongod({ remember: true, binVersion: oldVersion, smallfiles: "" });
var err = conn.getDB(dbName)[collName].ensureIndex({real: "2d"});
assert.eq(err, undefined);
err = conn.getDB(dbName)[collName].ensureIndex({fake: "2dsphere"});
assert.eq(err, undefined);
err = conn.getDB(dbName)[collName].ensureIndex({bad: "asdf"});
assert.eq(err, undefined);

// Add some data
conn.getDB(dbName)[collName].insert({real: [1,2], fake: [1,2], gibberish: 1});
assert.isnull(conn.getDB(dbName).getLastError());

// bad indexes treated as ascending
assert.matches(/BtreeCursor fake_2d/,  conn.getDB(dbName)[collName].find({fake: [1,2]}).explain());
assert.matches(/BtreeCursor bad_asdf/, conn.getDB(dbName)[collName].find({bad: 1}).explain());

////////////////////////////
conn = restartWithNew(conn);
////////////////////////////

// Make sure we warn at startup
var res = conn.adminCommand( { getLog : "startupWarnings" } );
assert.commandWorked(res);
var startupRegex = /Index .* claims to be of type /;
assert.matches(startupRegex, res);

// bad indexes treated still as ascending
assert.matches(/BtreeCursor fake_2d/, conn.getDB(dbName)[collName].find({fake: [1,2]}).explain());
assert.matches(/BtreeCursor bad_asdf/, conn.getDB(dbName)[collName].find({bad: 1}).explain());

// Can't create a real 2dsphere now
err = conn.getDB(dbName)[collName].ensureIndex({real: "2dsphere"});
assert(err);
// error code 67 = CannotCreateIndex
assert.eq(err.code, 67);

// Can create real btree index
var err = conn.getDB(dbName)[collName].ensureIndex({realbtree: 1});
assert.eq(err, undefined);

// Can create real 2d index
var err = conn.getDB(dbName)[collName].ensureIndex({realbtree: "2d"});
assert.eq(err, undefined);

// Can create real hashed index
var err = conn.getDB(dbName)[collName].ensureIndex({realHashed: "hashed"});
assert.eq(err, undefined);

// Can still downgrade
////////////////////////////
conn = restartWithOld(conn);
////////////////////////////

// bad indexes treated still as ascending
assert.matches(/BtreeCursor fake_2d/, conn.getDB(dbName)[collName].find({fake: [1,2]}).explain());
assert.matches(/BtreeCursor bad_asdf/, conn.getDB(dbName)[collName].find({bad: 1}).explain());

////////////////////////////
conn = restartWithNew(conn);
////////////////////////////

// Can drop bad indexes
assert.commandWorked(conn.getDB(dbName)[collName].dropIndex({fake: "2dsphere"}));
assert.commandWorked(conn.getDB(dbName)[collName].dropIndex({bad: "asdf"}));

// Can now build real index
err = conn.getDB(dbName)[collName].ensureIndex({real: "2dsphere"});
assert.eq(err, undefined);

MongoRunner.stopMongod(conn);

// Once you created a real 2dsphere, can't downgrade
////////////////////////////
badConn = restartWithOld(conn);
////////////////////////////
assert.isnull(badConn);

////////////////////////////
conn = restartWithNew(conn);
////////////////////////////

// Make sure we don't warn at startup
var res = conn.adminCommand( { getLog : "startupWarnings" } );
assert.commandWorked(res);
assert.noMatches(startupRegex, res);

MongoRunner.stopMongod(conn);
