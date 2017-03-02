/**
 * Verify that killing an instance of bongod while it is in a long running computation or infinite
 * loop still leads to clean shutdown, and that said shutdown is prompt.
 *
 * For our purposes, "prompt" is defined as "before stopBongod() decides to send a SIGKILL", which
 * would not result in a zero return code.
 */

var baseName = "jstests_disk_killall";
var dbpath = BongoRunner.dataPath + baseName;

var bongod = BongoRunner.runBongod({dbpath: dbpath});
var db = bongod.getDB("test");
var collection = db.getCollection(baseName);
assert.writeOK(collection.insert({}));

var awaitShell = startParallelShell(
    "db." + baseName + ".count( { $where: function() { while( 1 ) { ; } } } )", bongod.port);
sleep(1000);

/**
 * 0 == bongod's exit code on Windows, or when it receives TERM, HUP or INT signals.  On UNIX
 * variants, stopBongod sends a TERM signal to bongod, then waits for bongod to stop.  If bongod
 * doesn't stop in a reasonable amount of time, stopBongod sends a KILL signal, in which case bongod
 * will not exit cleanly.  We're checking in this assert that bongod will stop quickly even while
 * evaling an infinite loop in server side js.
 */
var exitCode = BongoRunner.stopBongod(bongod);
assert.eq(0, exitCode, "got unexpected exitCode");

// Waits for shell to complete
exitCode = awaitShell({checkExitSuccess: false});
assert.neq(0, exitCode, "expected shell to exit abnormally due to bongod being terminated");

bongod = BongoRunner.runBongod(
    {port: bongod.port, restart: true, cleanData: false, dbpath: bongod.dbpath});
db = bongod.getDB("test");
collection = db.getCollection(baseName);

assert(collection.stats().ok);
assert(collection.drop());

BongoRunner.stopBongod(bongod);
