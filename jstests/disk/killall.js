/**
 * Verify that killing an instance of mongod while it is in a long running computation or infinite
 * loop still leads to clean shutdown, and that said shutdown is prompt.
 *
 * For our purposes, "prompt" is defined as "before stopMongod() decides to send a SIGKILL", which
 * would not result in a zero return code.
 */

var baseName = "jstests_disk_killall";
var dbpath = MongoRunner.dataPath + baseName;

var mongod = MongoRunner.runMongod({dbpath: dbpath});
var db = mongod.getDB("test");
var collection = db.getCollection(baseName);
assert.writeOK(collection.insert({}));

var awaitShell = startParallelShell(
    "db." + baseName + ".count( { $where: function() { while( 1 ) { ; } } } )", mongod.port);
sleep(1000);

/**
 * 0 == mongod's exit code on Windows, or when it receives TERM, HUP or INT signals.  On UNIX
 * variants, stopMongod sends a TERM signal to mongod, then waits for mongod to stop.  If mongod
 * doesn't stop in a reasonable amount of time, stopMongod sends a KILL signal, in which case mongod
 * will not exit cleanly.  We're checking in this assert that mongod will stop quickly even while
 * evaling an infinite loop in server side js.
 */
var exitCode = MongoRunner.stopMongod(mongod);
assert.eq(0, exitCode, "got unexpected exitCode");

// Waits for shell to complete
exitCode = awaitShell({checkExitSuccess: false});
assert.neq(0, exitCode, "expected shell to exit abnormally due to mongod being terminated");

mongod = MongoRunner.runMongod(
    {port: mongod.port, restart: true, cleanData: false, dbpath: mongod.dbpath});
db = mongod.getDB("test");
collection = db.getCollection(baseName);

assert(collection.stats().ok);
assert(collection.drop());

MongoRunner.stopMongod(mongod);
