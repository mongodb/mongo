// Test checksum validation of journal files.

var testname = "dur_checksum";
var path = BongoRunner.dataPath + testname;

if (0) {
    // This is used to create the prototype journal file.
    jsTest.log("Just creating prototype journal, not testing anything");
    var conn = BongoRunner.runBongod({dbpath: path, journal: ""});
    var db = conn.getDB("test");

    // each insert is in it's own commit.
    db.foo.insert({a: 1});
    db.runCommand({getlasterror: 1, j: 1});

    db.foo.insert({a: 2});
    db.runCommand({getlasterror: 1, j: 1});

    BongoRunner.stopBongod(conn.port, /*signal*/ 9);

    jsTest.log("Journal file left at " + path + "/journal/j._0");
    quit();
    // A hex editor must be used to replace the checksums of specific journal sections with
    // "0BADC0DE 1BADC0DE 2BADC0DE 3BADCD0E"
}

function startBongodWithJournal() {
    return BongoRunner.runBongod({
        restart: true,
        cleanData: false,
        dbpath: path,
        journal: "",
        smallfiles: "",
        journalOptions: 1 /*DurDumpJournal*/
    });
}

jsTest.log("Starting with good.journal to make sure everything works");
resetDbpath(path);
mkdir(path + '/journal');
copyFile("jstests/libs/dur_checksum_good.journal", path + "/journal/j._0");
var conn = startBongodWithJournal();
var db = conn.getDB('test');
assert.eq(db.foo.count(), 2);
BongoRunner.stopBongod(conn.port);

// dur_checksum_bad_last.journal is good.journal with the bad checksum on the last section.
jsTest.log("Starting with bad_last.journal");
resetDbpath(path);
mkdir(path + '/journal');
copyFile("jstests/libs/dur_checksum_bad_last.journal", path + "/journal/j._0");
conn = startBongodWithJournal();
var db = conn.getDB('test');
assert.eq(db.foo.count(), 1);  // 2nd insert "never happened"
BongoRunner.stopBongod(conn.port);

// dur_checksum_bad_first.journal is good.journal with the bad checksum on the prior section.
// This means there is a good commit after the bad one. We currently ignore this, but a future
// version of the server may be able to detect this case.
jsTest.log("Starting with bad_first.journal");
resetDbpath(path);
mkdir(path + '/journal');
copyFile("jstests/libs/dur_checksum_bad_first.journal", path + "/journal/j._0");
conn = startBongodWithJournal();
var db = conn.getDB('test');
assert.eq(db.foo.count(), 0);  // Neither insert happened.
BongoRunner.stopBongod(conn.port);

// If we detect an error in a non-final journal file, that is considered an error.
jsTest.log("Starting with bad_last.journal followed by good.journal");
resetDbpath(path);
mkdir(path + '/journal');
copyFile("jstests/libs/dur_checksum_bad_first.journal", path + "/journal/j._0");
copyFile("jstests/libs/dur_checksum_good.journal", path + "/journal/j._1");

exitCode = runBongoProgram("bongod",
                           "--port",
                           allocatePort(),
                           "--dbpath",
                           path,
                           "--journal",
                           "--smallfiles",
                           "--journalOptions",
                           1 /*DurDumpJournal*/
                               +
                               2 /*DurScanOnly*/);

assert.eq(exitCode, 100 /*EXIT_UNCAUGHT*/);

// TODO Possibly we could check the bongod log to verify that the correct type of exception was
// thrown.  But that would introduce a dependency on the bongod log format, which we may not want.

jsTest.log("SUCCESS checksum.js");
