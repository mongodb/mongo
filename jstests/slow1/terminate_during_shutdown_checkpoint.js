/**
 * Terminates mongod during a clean shutdown.
 *
 * As part of a clean shutdown, WiredTiger takes a final checkpoint. Terminating mongod during this
 * process results in an unclean shutdown. During startup recovery, WiredTiger will use the last
 * successful checkpoint taken and replay any journal files. Replication recovery will replay any
 * operations beyond the stable timestamp from the oplog.
 *
 * This test is verifying that startup recovery works as designed following an unclean shutdown
 * during the closing checkpoint.
 *
 * @tags: [requires_wiredtiger, requires_persistence, requires_replication]
 */
(function() {
"use strict";

// This test triggers an unclean shutdown, which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;

load("jstests/libs/parallel_shell_helpers.js");  // For startParallelShell
load('jstests/libs/parallelTester.js');          // For Thread

const kSeparator = "/";
const copyDataFiles = function(src, dest) {
    ls(src).forEach((file) => {
        if (file.endsWith(kSeparator)) {
            return;
        }

        let fileName = file.substring(file.lastIndexOf(kSeparator) + 1);
        copyFile(file, dest + kSeparator + fileName);
    });

    ls(src + kSeparator + "journal").forEach((file) => {
        let fileName = file.substring(file.lastIndexOf(kSeparator) + 1);
        copyFile(file, dest + kSeparator + "journal" + kSeparator + fileName);
    });
};

// Multi-document prepared transaction workload executed by the client threads.
const threadWorkload = function(host, threadId, start, end) {
    load("jstests/core/txns/libs/prepare_helpers.js");

    try {
        const conn = new Mongo(host);

        let session;
        let sessionDB;
        let sessionColl;

        for (let i = start; i < end; i++) {
            if (session == undefined) {
                session = conn.startSession({causalConsistency: false});
                sessionDB = session.getDatabase("test");
                sessionColl = sessionDB.getCollection("foo");

                jsTestLog("ThreadId: " + threadId + ", starting transaction");
                session.startTransaction();
            }

            const docId = Math.floor(Math.random() * (end - start + 1)) + start;

            jsTestLog("ThreadId: " + threadId + ", inserting document with _id " +
                      docId.toString());
            const insertRes = sessionColl.insert({
                _id: docId,
                a: docId,
                b: docId,
                c: docId,
                d: docId,
                e: docId,
            });

            if (insertRes.nInserted == 0) {
                // DuplicateKey error using the generated docId for _id. Start a new transaction.
                session = undefined;
                continue;
            }

            if (i % 2 == 0) {
                assert.commandWorked(sessionColl.update({_id: docId}, {$unset: {a: 1}}));
                assert.commandWorked(sessionColl.update({_id: docId}, {$set: {a: i}}));
                assert.commandWorked(sessionColl.update({_id: docId}, {$inc: {b: 1}}));
                assert.commandWorked(sessionColl.update({_id: docId}, {$inc: {c: 2}}));
                assert.commandWorked(sessionColl.update({_id: docId}, {$inc: {d: 3}}));
                assert.commandWorked(sessionColl.update({_id: docId}, {$set: {e: i * i}}));
            }

            if (i % 4 == 0) {
                assert.commandWorked(sessionColl.deleteOne({_id: docId}));
            }

            if (i % 25 == 0) {
                jsTestLog("ThreadId: " + threadId + ", preparing transaction");
                let prepareTimestamp = PrepareHelpers.prepareTransaction(session);
                if (i % 50 == 0) {
                    // Only commit half of the prepared transactions.
                    jsTestLog("ThreadId: " + threadId + ", committing prepared transaction");
                    PrepareHelpers.commitTransaction(session, prepareTimestamp);
                } else {
                    jsTestLog("ThreadId: " + threadId +
                              ", leaving prepared transaction uncommitted");
                }
                session = undefined;
            }

            if (i % 1000 == 0) {
                jsTestLog("ThreadId: " + threadId + ", Progress: " + (i - start) + "/" +
                          (end - start));
            }
        }
    } catch (e) {
        // The shell may throw after mongod was terminated.
    }
};

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        syncdelay: 10,  // Fast checkpoint intervals.
        slowms: 30000,  // Don't log slow queries.
        setParameter: {logComponentVerbosity: tojsononeline({storage: {recovery: 2}})},
        wiredTigerEngineConfigString:
            "debug_mode=(slow_checkpoint=true,table_logging=true),verbose=[checkpoint,checkpoint_cleanup,checkpoint_progress]"
    }
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

const dbName = "test";
const collName = "foo";

let db = primary.getDB(dbName);
assert.commandWorked(db.createCollection(collName));
let coll = db.getCollection(collName);

// Create many indexes to have more table files.
assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}, {c: 1}, {d: 1}, {e: 1}]));

const kNumUncleanShutdowns = 5;
let numUncleanShutdowns = 0;
while (numUncleanShutdowns < kNumUncleanShutdowns) {
    jsTestLog("Execution #" + numUncleanShutdowns);

    const kOpsPerThread = 1000000;
    const kNumThreads = 10;
    let threads = [];
    for (let i = 0; i < kNumThreads; i++) {
        let thread = new Thread(threadWorkload,
                                primary.host,
                                i,
                                i * kOpsPerThread,
                                kOpsPerThread + (i * kOpsPerThread));
        threads.push(thread);
        thread.start();
    }

    // Wait between 30 to 60 seconds before shutting down mongod.
    const secsToWait = Math.floor(Math.random() * 30) + 30;
    jsTestLog("Waiting " + secsToWait + " seconds before shutting down");
    sleep(secsToWait * 1000);

    // Start a clean shutdown.
    clearRawMongoProgramOutput();
    const awaitShutdown = startParallelShell(() => {
        db.adminCommand({shutdown: 1});
    }, primary.port);

    assert.soon(() => {
        const logContents = rawMongoProgramOutput();
        return logContents.indexOf("close_ckpt") > 0;
    });

    // During the shutdown checkpoint, kill the process.
    const pid = primary.pid;
    const kSIGKILL = 9;
    jsTestLog("Killing pid: " + pid);
    stopMongoProgramByPid(pid, kSIGKILL);
    jsTestLog("Killed pid: " + pid);

    const exitCode = awaitShutdown({checkExitSuccess: false});
    assert.neq(0, exitCode, 'Expected shell to exit abnormally due to shutdown');

    numUncleanShutdowns++;

    threads.forEach(function(thread) {
        thread.join();
    });

    // Make a copy of the data files after terminating the shutdown. This is helpful for
    // investigations when there is a failure.
    const copyPath = primary.dbpath + kSeparator + "exec" + numUncleanShutdowns.toString();
    resetDbpath(copyPath);
    mkdir(copyPath + kSeparator + "journal");
    copyDataFiles(primary.dbpath, copyPath);

    rst.restart(primary);

    primary = rst.getPrimary();
    db = primary.getDB(dbName);
    coll = db.getCollection(collName);
}

rst.stopSet();
}());
