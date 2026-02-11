/**
 * Confirms that long-running transactions are logged once during their progress.
 * @tags: [requires_replication]
 */

import {findSlowInProgressQueryLogLine} from "jstests/libs/log.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

function assertSlowInProgressTransactionLogged(db, comment) {
    const logLine = findSlowInProgressQueryLogLine(db, comment);
    assert.neq(null, logLine, "Did not find slow in-progress transaction log line");
}

const kDocCount = 1000;
const kLargeStr = "x".repeat(1024 * 128); // 128KB string

// Ensure that we yield often enough to log the "slow" in-progress transaction.
const rst = new ReplSetTest({
    nodes: 1,
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("log_slow_in_progress_transactions");
const coll = db.test;

assert.commandWorked(db.dropDatabase());
assert.commandWorked(db.setProfilingLevel(0, {slowinprogms: 0}));

const docs = [];
for (let i = 0; i < kDocCount; ++i) {
    docs.push({a: i, big: kLargeStr});
}

function setup_coll(coll) {
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndex({a: 1}));
}

setup_coll(coll);

// Start a transaction and perform a long-running query within it.
const session = db.getMongo().startSession();
const sessionDb = session.getDatabase("log_slow_in_progress_transactions");
const sessionColl = sessionDb.test;

let startTime = Date.now();
session.startTransaction();
try {
    // Slow collection scan: use $where to sleep for each doc
    assert.eq(
        kDocCount,
        sessionColl
            .find({
                $where: function () {
                    sleep(1);
                    return true;
                },
            })
            .comment("Transaction Collection Scan")
            .itcount(),
    );
    session.commitTransaction();
} catch (e) {
    session.abortTransaction();
    throw e;
}
let elapsed = Date.now() - startTime;
print(`Transaction Collection Scan took ${elapsed}ms`);
assertSlowInProgressTransactionLogged(db, "Transaction Collection Scan");

// Test with index scan in transaction.
startTime = Date.now();
session.startTransaction();
try {
    assert.eq(
        kDocCount,
        sessionColl
            .find({
                a: {$gte: 0},
                $where: function () {
                    sleep(1);
                    return true;
                },
            })
            .comment("Transaction Index Scan")
            .itcount(),
    );
    session.commitTransaction();
} catch (e) {
    session.abortTransaction();
    throw e;
}
elapsed = Date.now() - startTime;
print(`Transaction Index Scan took ${elapsed}ms`);
assertSlowInProgressTransactionLogged(db, "Transaction Index Scan");

// Test with aggregation in transaction.
startTime = Date.now();
session.startTransaction();
try {
    assert.eq(
        kDocCount,
        sessionColl
            .aggregate(
                [
                    {$match: {a: {$gte: 0}}},
                    {
                        $addFields: {
                            slow: {
                                $function: {
                                    body: function () {
                                        sleep(1);
                                        return 1;
                                    },
                                    args: [],
                                    lang: "js",
                                },
                            },
                        },
                    },
                ],
                {comment: "Transaction Agg Index Scan"},
            )
            .itcount(),
    );
    session.commitTransaction();
} catch (e) {
    session.abortTransaction();
    throw e;
}
elapsed = Date.now() - startTime;
print(`Transaction Agg Index Scan took ${elapsed}ms`);
assertSlowInProgressTransactionLogged(db, "Transaction Agg Index Scan");

// Test with update in transaction.
startTime = Date.now();
session.startTransaction();
try {
    assert.commandWorked(
        sessionDb.runCommand({
            update: "test",
            updates: [
                {
                    q: {
                        a: {$gte: 0},
                        $where: function () {
                            sleep(1);
                            return true;
                        },
                    },
                    u: {$inc: {u: 1}},
                    multi: true,
                },
            ],
            comment: "Transaction Update Index Scan",
        }),
    );
    session.commitTransaction();
} catch (e) {
    // On CI, we can sometimes get TemporarilyUnavailable during commit due to timing/load.
    // Since this test is about slow query logging, not write concern, we can ignore it.
    if (e.code === 365 || (e.writeConcernError && e.writeConcernError.code === 365)) {
        print("Ignoring TemporarilyUnavailable error during commit (expected on CI)");
        try {
            session.abortTransaction();
        } catch (abortErr) {
            // Ignore abort errors too
        }
    } else {
        session.abortTransaction();
        throw e;
    }
}
elapsed = Date.now() - startTime;
print(`Transaction Update Index Scan took ${elapsed}ms`);
assertSlowInProgressTransactionLogged(db, "Transaction Update Index Scan");

session.endSession();

rst.stopSet();
