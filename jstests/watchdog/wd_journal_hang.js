// Storage Node Watchdog - validate watchdog monitors --dbpath /journal
// @tags: [requires_persistence]
//
load("jstests/watchdog/lib/wd_test_common.js");

(function() {
'use strict';

function trimTrailingSlash(dir) {
    if (dir.endsWith('/')) {
        return dir.substring(0, dir.length - 1);
    }

    return dir;
}

let control = new CharybdefsControl("journalpath_hang");

const journalFusePath = control.getMountPath();

const dbPath = MongoRunner.toRealDir("$dataDir/mongod-journal");

const journalLinkPath = dbPath + "/journal";

resetDbpath(dbPath);

// Create a symlink from the non-fuse journal directory to the fuse mount.
const ret = run("ln", "-s", trimTrailingSlash(journalFusePath), journalLinkPath);
assert.eq(ret, 0);

// Set noCleanData so that the dbPath is not cleaned because we want to use the journal symlink.
testFuseAndMongoD(control, {dbpath: dbPath, noCleanData: true});
})();
