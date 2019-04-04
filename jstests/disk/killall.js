/**
 * Verify that killing an instance of merizod while it is in a long running computation or infinite
 * loop still leads to clean shutdown, and that said shutdown is prompt.
 *
 * For our purposes, "prompt" is defined as "before stopMerizod() decides to send a SIGKILL", which
 * would not result in a zero return code.
 */

var baseName = "jstests_disk_killall";
var dbpath = MerizoRunner.dataPath + baseName;

var merizod = MerizoRunner.runMerizod({dbpath: dbpath});
var db = merizod.getDB("test");
var collection = db.getCollection(baseName);
assert.writeOK(collection.insert({}));

var awaitShell = startParallelShell(
    "db." + baseName + ".count( { $where: function() { while( 1 ) { ; } } } )", merizod.port);
sleep(1000);

/**
 * 0 == merizod's exit code on Windows, or when it receives TERM, HUP or INT signals.  On UNIX
 * variants, stopMerizod sends a TERM signal to merizod, then waits for merizod to stop.  If merizod
 * doesn't stop in a reasonable amount of time, stopMerizod sends a KILL signal, in which case merizod
 * will not exit cleanly.  We're checking in this assert that merizod will stop quickly even while
 * evaling an infinite loop in server side js.
 */
var exitCode = MerizoRunner.stopMerizod(merizod);
assert.eq(0, exitCode, "got unexpected exitCode");

// Waits for shell to complete
exitCode = awaitShell({checkExitSuccess: false});
assert.neq(0, exitCode, "expected shell to exit abnormally due to merizod being terminated");

merizod = MerizoRunner.runMerizod(
    {port: merizod.port, restart: true, cleanData: false, dbpath: merizod.dbpath});
db = merizod.getDB("test");
collection = db.getCollection(baseName);

assert(collection.stats().ok);
assert(collection.drop());

MerizoRunner.stopMerizod(merizod);
