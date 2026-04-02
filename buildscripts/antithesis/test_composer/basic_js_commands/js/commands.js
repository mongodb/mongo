const MONGOD_URI = connection_string;

const MAX_RETRIES = 180; // 3 minutes

// Executes the given function, retrying if it fails
// due to the network being unreachable (MongoNetworkError)
// or cluster unavailable (MongoServerSelectionError)
// or retryable write error.
function retryOnFailure(func) {
    let retries = 0;
    while (true) {
        try {
            const result = func();
            return result;
        } catch (err) {
            if (retries++ >= MAX_RETRIES) {
                print(`failed (exhausted all retries): ${JSON.stringify(err)}`);
                throw err;
            }
            if (
                err.name === "MongoServerSelectionError" ||
                err.name === "MongoNetworkError" ||
                err.name === "MongoNetworkTimeoutError" ||
                err.name === "MongoPoolClearedError" ||
                err.name === "PoolClearedOnNetworkError" ||
                err.name === "InterruptedDueToReplStateChange" ||
                err.message === "read ECONNRESET" ||
                err.message === "read ETIMEDOUT" ||
                err.message === "Shutting down" ||
                err.message.startsWith("network error while attempting to run command")
            ) {
                print(`Attempt ${retries} failed due to ${err.name}, retrying in 1 second...`);
                sleep(1_000);
                continue;
            }
            if (
                Array.isArray(err.errorResponse?.errorLabels) &&
                err.errorResponse.errorLabels.includes("RetryableWriteError")
            ) {
                print(`Attempt ${retries} failed due to ${err.codeName}, retrying in 1 second...`);
                sleep(1_000);
                continue;
            }

            // Some other non-retryable issue, re-throw
            print(`failed due to unretryable error: ${JSON.stringify(err)}`);
            throw err;
        }
    }
}

// Exclusive upper bound (0 to max-1)
function randomInt(max) {
    return Math.floor(Math.random() * max);
}

// Establishes a connection
function getDB() {
    return retryOnFailure(() => new Mongo(MONGOD_URI).getDB("test"));
}

function find() {
    const db = getDB();
    print("executing find");
    for (let i = 0; i < 100; i++) {
        const val = randomInt(10_000) + 1;
        retryOnFailure(() => db.test.findOne({x: {$gt: val}}));
    }
    print("done executing find");
}

function fsync() {
    const db = getDB();
    print("executing fsync");
    const result = retryOnFailure(() => db.adminCommand({fsync: 1}));
    print("done executing fsync. result:", JSON.stringify(result));
    assert(result.ok);
}

function insert() {
    const db = getDB();
    for (let i = 0; i < 100; i++) {
        const val = randomInt(10_000) + 1;
        retryOnFailure(() => db.test.insertOne({x: val}));
    }
}

// Executes a write, then a point-in-time read
// to fetch the older state of the doc prior to the write.
function pitRead() {
    // Set up initial state, a doc with field "value":0
    const db = getDB();
    const _id = ObjectId();
    retryOnFailure(() => {
        db.createCollection("test_snapshots");
        db.test_snapshots.updateOne({_id}, {$set: {value: 0}}, {upsert: true, writeConcern: {w: "majority"}});
    });

    // Execute an update (set value=1) and capture its timestamp as t1
    const t1 = retryOnFailure(() => {
        const ses1 = db.getMongo().startSession();
        const ses1Coll = ses1.getDatabase(db.getName()).test_snapshots;
        ses1Coll.updateOne({_id}, {$set: {value: 1}}, {writeConcern: {w: "majority"}});
        const t1 = ses1.getOperationTime();
        db.test_snapshots.updateOne({_id}, {$set: {value: 2}}, {writeConcern: {w: "majority"}});
        return t1;
    });

    // Sleep for a random period between 0 and 30 seconds.
    // The snapshot history window is configured to 10 seconds,
    // so sometimes this will attempt a read outside the window.
    sleep(randomInt(30) * 1000);

    let snapshotReadResult;
    try {
        snapshotReadResult = retryOnFailure(() => {
            // Do a snapshot read to get the document's state as of t1
            return db.runCommand({
                find: "test_snapshots",
                filter: {_id},
                readConcern: {level: "snapshot", atClusterTime: t1},
            });
        });
    } catch (e) {
        if (e.codeName === "SnapshotTooOld") {
            // This case is expected sometimes, so exit cleanly.
            return;
        }
        throw e;
    }

    // Assert that the snapshot read actually returned the version of the
    // doc at the older value (1), not the newer one (2).
    assert(JSON.stringify(snapshotReadResult.cursor.firstBatch) == JSON.stringify([{_id, value: 1}]));
}

function validateCollections() {
    const db = getDB();
    const dbs = retryOnFailure(() => db.adminCommand("listDatabases").databases.map((x) => x.name));
    dbs.forEach((dbName) => {
        const collectionNames = retryOnFailure(() => db.getSiblingDB(dbName).getCollectionNames());
        collectionNames.forEach((coll) => {
            print(`validating ${dbName}.${coll}...`);
            const validateResult = retryOnFailure(() => db.getSiblingDB(dbName).getCollection(coll).validate());
            assert(validateResult.valid, "collection is not valid");
        });
    });
}

function aggregate() {
    const db = getDB();
    print("executing aggregate");

    const pipelines = [
        // $group with $sum and $avg
        [{$group: {_id: null, total: {$sum: "$x"}, avg: {$avg: "$x"}, count: {$sum: 1}}}],
        // $match + $sort + $project + $limit
        [{$match: {x: {$gt: randomInt(5_000)}}}, {$sort: {x: -1}}, {$project: {_id: 0, x: 1}}, {$limit: 10}],
        // $group by bucket
        [{$group: {_id: {$mod: ["$x", 10]}, count: {$sum: 1}}}, {$sort: {_id: 1}}],
        // $match + $group with $min/$max
        [{$match: {x: {$exists: true}}}, {$group: {_id: null, max: {$max: "$x"}, min: {$min: "$x"}}}],
        // $lookup (self-join) + $unwind
        [
            {$limit: 5},
            {$lookup: {from: "test", localField: "x", foreignField: "x", as: "matches"}},
            {$unwind: {path: "$matches", preserveNullAndEmptyArrays: true}},
            {$project: {x: 1, matchX: "$matches.x"}},
        ],
    ];

    for (const pipeline of pipelines) {
        retryOnFailure(() => db.test.aggregate(pipeline).toArray());
    }
    print("done executing aggregate");
}

function update() {
    const db = getDB();
    print("executing update");
    for (let i = 0; i < 50; i++) {
        const val = randomInt(10_000) + 1;
        const op = randomInt(3);
        if (op === 0) {
            retryOnFailure(() => db.test.updateOne({x: {$gt: val}}, {$set: {x: randomInt(10_000) + 1}}));
        } else if (op === 1) {
            retryOnFailure(() => db.test.updateMany({x: {$lt: val}}, {$inc: {x: 1}}));
        } else {
            retryOnFailure(() => db.test.updateOne({x: val}, {$push: {tags: `tag_${randomInt(5)}`}}, {upsert: true}));
        }
    }
    print("done executing update");
}

function deleteOp() {
    const db = getDB();
    print("executing delete");
    for (let i = 0; i < 20; i++) {
        const val = randomInt(10_000) + 1;
        if (randomInt(2) === 0) {
            retryOnFailure(() => db.test.deleteOne({x: val}));
        } else {
            // deleteMany with a tight filter to avoid wiping too many docs
            retryOnFailure(() => db.test.deleteMany({x: {$lt: Math.floor(val / 1000)}}));
        }
    }
    print("done executing delete");
}

function txn() {
    const db = getDB();
    print("executing txn");

    const MAX_TXN_RETRIES = 30;

    const commitWithRetry = (session) => {
        let retries = 0;
        while (true) {
            try {
                session.commitTransaction();
                return;
            } catch (err) {
                if (
                    retries++ < MAX_TXN_RETRIES &&
                    Array.isArray(err.errorResponse?.errorLabels) &&
                    err.errorResponse.errorLabels.includes("UnknownTransactionCommitResult")
                ) {
                    print(`commit attempt ${retries} got UnknownTransactionCommitResult, retrying...`);
                    sleep(1_000);
                    continue;
                }
                throw err;
            }
        }
    };

    let txnRetries = 0;
    while (true) {
        const session = db.getMongo().startSession();
        session.startTransaction({
            readConcern: {level: "snapshot"},
            writeConcern: {w: "majority"},
        });
        try {
            const sessionDb = session.getDatabase("test");
            const val = randomInt(10_000) + 1;
            sessionDb.test.insertOne({x: val, txn: true});
            const found = sessionDb.test.findOne({x: {$gt: Math.floor(val / 2)}});
            if (found) {
                sessionDb.test.updateOne({_id: found._id}, {$set: {updated: true}});
            }
            commitWithRetry(session);
            session.endSession();
            print("txn committed");
            break;
        } catch (err) {
            try {
                session.abortTransaction();
            } catch (_) {}
            session.endSession();

            const isTransient =
                Array.isArray(err.errorResponse?.errorLabels) &&
                err.errorResponse.errorLabels.includes("TransientTransactionError");
            const isNetwork =
                err.name === "MongoServerSelectionError" ||
                err.name === "MongoNetworkError" ||
                err.name === "MongoNetworkTimeoutError" ||
                err.name === "MongoPoolClearedError";

            if (txnRetries++ < MAX_TXN_RETRIES && (isTransient || isNetwork)) {
                print(`txn attempt ${txnRetries} failed (${err.name ?? err.codeName}), retrying...`);
                sleep(1_000);
                continue;
            }
            throw err;
        }
    }
    print("done executing txn");
}

function dbcheck() {
    const db = getDB();
    print("executing dbcheck");
    const collectionNames = retryOnFailure(() => db.getCollectionNames());
    for (const coll of collectionNames) {
        print(`running dbCheck on test.${coll}`);
        retryOnFailure(() => {
            const result = db.runCommand({dbCheck: coll});
            assert(result.ok, `dbCheck failed for test.${coll}: ${JSON.stringify(result)}`);
        });
    }
    print("done executing dbcheck");
}
