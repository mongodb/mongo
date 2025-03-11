/**
 * Tests that the size storer entry for a collection gets cleaned up when that collection is
 * dropped.
 *
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */

import {
    getUriForColl,
    getUriForIndex,
    runWiredTigerTool,
} from "jstests/disk/libs/wt_file_helper.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();
const dbpath = primary.dbpath;

const coll = function() {
    return primary.getDB(jsTestName()).test;
};

assert.commandWorked(coll().insert({a: 1}));
assert.eq(coll().count(), 1);
const uri = coll().stats().wiredTiger.uri.split("statistics:")[1];

const assertSizeStorerEntry = function(expected) {
    const filePath = dbpath + (_isWindows() ? "\\" : "/") + jsTestName();
    runWiredTigerTool("-r", "-h", dbpath, "dump", "-j", "-k", uri, "-f", filePath, "sizeStorer");
    const data = JSON.parse(cat(filePath))["table:sizeStorer"][1].data;
    assert.eq(data.length, expected ? 1 : 0, tojson(data));
};

replTest.stop(primary, undefined, {}, {forRestart: true});
assertSizeStorerEntry(true);

replTest.start(primary,
               {
                   setParameter: {
                       minSnapshotHistoryWindowInSeconds: 0,
                       // Set storage logComponentVerbosity to one so we see the log id 22237.
                       logComponentVerbosity: tojson({storage: 1})
                   }
               },
               true /* forRestart */);
primary = replTest.getPrimary();

const collIdent = getUriForColl(coll());
const indexIdent = getUriForIndex(coll(), "_id_");

assert.eq(coll().count(), 1);
assert(coll().drop());
assert.commandWorked(primary.adminCommand({appendOplogNote: 1, data: {msg: "advance timestamp"}}));
assert.commandWorked(primary.adminCommand({fsync: 1}));

checkLog.containsJson(primary, 22237, {ident: collIdent});
checkLog.containsJson(primary, 22237, {ident: indexIdent});

replTest.stop(primary, undefined, {}, {forRestart: true});
assertSizeStorerEntry(false);

replTest.start(primary, {}, true /* forRestart */);
replTest.stopSet();
