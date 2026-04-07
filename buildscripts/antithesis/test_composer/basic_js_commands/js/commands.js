const MONGO_URI = connection_string.includes("/") ? connection_string : `mongodb://${connection_string}`;

const MAX_RETRIES = 180; // 3 minutes

// All error codes with the RetriableError category from src/mongo/base/error_codes.yml and other
// error codes that we want to retry on.
const RETRYABLE_ERROR_CODES = new Set([
    6, // HostUnreachable
    7, // HostNotFound
    50, // MaxTimeMSExpired
    89, // NetworkTimeout
    91, // ShutdownInProgress
    112, // WriteConflict
    133, // FailedToSatisfyReadPreference
    134, // ReadConcernMajorityNotAvailableYet
    189, // PrimarySteppedDown
    246, // SnapshotUnavailable
    262, // ExceededTimeLimit
    317, // ConnectionPoolExpired
    358, // InternalTransactionNotSupported
    384, // ConnectionError
    402, // ResourceExhausted
    406, // MigrationBlockingOperationCoordinatorCleaningUp
    407, // PooledConnectionAcquisitionExceededTimeLimit
    412, // UpdatesStillPending
    453, // InterruptedDueToReshardingCriticalSection
    462, // IngressRequestRateLimitExceeded
    471, // InterruptedDueToAddShard
    479, // IFRFlagRetry
    485, // InterruptedDueToTimeseriesUpgradeDowngrade
    488, // PrimaryOnlyServiceInitializing
    9001, // SocketException
    10107, // NotWritablePrimary
    11600, // InterruptedAtShutdown
    11602, // InterruptedDueToReplStateChange
    13435, // NotPrimaryNoSecondaryOk
    13436, // NotPrimaryOrSecondary
    50915, // BackupCursorOpenConflictWithCheckpoint
    91331, // RetriableRemoteCommandFailure
    10045600, // InterruptedDueToFCVChange
]);

function isRetryableError(code, message = "") {
    return (
        RETRYABLE_ERROR_CODES.has(code) ||
        message === "read ECONNRESET" ||
        message === "read ETIMEDOUT" ||
        message === "Shutting down" ||
        message.startsWith("socket exception") ||
        message.startsWith("can't connect") ||
        message.startsWith("network error while attempting to run command") ||
        message.startsWith("Could not find host matching read preference") ||
        message.startsWith("Could not satisfy $readPreference")
    );
}

// Executes the given function, retrying if it fails
// due to the network being unreachable (MongoNetworkError),
// cluster unavailable (MongoServerSelectionError),
// retryable write error, or any error code in RETRYABLE_ERROR_CODES.
function retryOnFailure(func) {
    for (let retries = 0; retries < MAX_RETRIES; ++retries) {
        try {
            const result = func();
            if (result && result.ok === 0) {
                // We can sometimes receive a response with ok:0 without getting an exception.
                print(`Received not OK result: ${JSON.stringify(result)}`);
                const code = result.code || result.all?.code || 0;
                const errmsg = result.errmsg || result.codeName || result.all?.codeName || "";
                if (isRetryableError(code, errmsg)) {
                    print(
                        `Attempt ${retries} failed due to retryable ok:0 result: "${JSON.stringify(result)}", retrying in 1 second...`,
                    );
                    sleep(1000);
                    continue;
                }
                // If it's not a retryable error, fall through and return the result, the caller is responsible for handling it.
            }
            return result;
        } catch (err) {
            if (
                isRetryableError(err.code, err.message) ||
                err.name === "MongoServerSelectionError" ||
                err.name === "MongoNetworkError" ||
                err.name === "MongoNetworkTimeoutError" ||
                err.name === "MongoPoolClearedError" ||
                err.name === "PoolClearedOnNetworkError" ||
                err.name === "WriteConcernError"
            ) {
                print(`Attempt ${retries} failed due to "${err.name}: ${err.message}", retrying in 1 second...`);
                sleep(1_000);
                continue;
            }

            const errLabels = err.errorLabels || err.errorResponse?.errorLabels || [];
            if (
                Array.isArray(errLabels) &&
                ["RetryableWriteError", "TransientTransactionError", "RetryableError"].some((label) =>
                    errLabels.includes(label),
                )
            ) {
                print(`Attempt ${retries} failed due to "${err.codeName}: ${err.message}", retrying in 1 second...`);
                sleep(1_000);
                continue;
            }

            // Some other non-retryable issue, re-throw
            print(`failed due to unretryable error: ${JSON.stringify(err)}`);
            throw err;
        }
    }

    print(`Exhausted all retries, failing`);
    throw Error(`Operation failed after ${MAX_RETRIES} attempts`);
}

// Exclusive upper bound (0 to max-1)
function randomInt(max) {
    return Math.floor(Math.random() * max);
}

// Establishes a connection
function getDB() {
    return retryOnFailure(() => {
        print(`Establishing connection to "${MONGO_URI}"`);
        const conn = new Mongo(MONGO_URI);
        conn.setReadPref("primaryPreferred");
        return conn.getDB("test");
    });
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

    print("done executing PIT read. result:", JSON.stringify(snapshotReadResult));
    // Assert that the snapshot read actually returned the version of the
    // doc at the older value (1), not the newer one (2).
    assert(
        JSON.stringify(snapshotReadResult.cursor.firstBatch) === JSON.stringify([{_id, value: 1}]),
        `PIT read returned unexpected result: ${JSON.stringify(snapshotReadResult)}`,
    );
}

function validateCollections() {
    const db = getDB();
    const dbs = retryOnFailure(() => db.adminCommand("listDatabases")).databases.map((x) => x.name);
    dbs.forEach((dbName) => {
        const collectionNames = retryOnFailure(() => db.getSiblingDB(dbName).getCollectionNames());
        collectionNames.forEach((coll) => {
            print(`validating ${dbName}.${coll}...`);
            const validateResult = retryOnFailure(() => db.getSiblingDB(dbName).getCollection(coll).validate());
            assert(
                validateResult.valid,
                `collection ${dbName}.${coll} is not valid: ${JSON.stringify(validateResult)}`,
            );
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
        retryOnFailure(() => db.test.aggregate(pipeline)).toArray();
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
            retryOnFailure(() => session.commitTransaction());
            session.endSession();
            print("txn committed");
            break;
        } catch (err) {
            try {
                session.abortTransaction();
                session.endSession();
            } catch (err) {
                print(`Caught exception while aborting transaction, ignoring: ${JSON.stringify(err)}`);
            }

            const isRetryableErr = isRetryableError(err.code, err.message);
            const isTransient =
                Array.isArray(err.errorResponse?.errorLabels) &&
                err.errorResponse.errorLabels.includes("TransientTransactionError");
            const isNetwork =
                err.name === "MongoServerSelectionError" ||
                err.name === "MongoNetworkError" ||
                err.name === "MongoNetworkTimeoutError" ||
                err.name === "MongoPoolClearedError";

            if (txnRetries++ < MAX_TXN_RETRIES && (isRetryableErr || isTransient || isNetwork)) {
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
        const result = retryOnFailure(() => db.runCommand({dbCheck: coll}));
        assert(result.ok, `dbCheck failed for test.${coll}: ${JSON.stringify(result)}`);
    }
    print("done executing dbcheck");
}

function replSetGetStatus() {
    const db = getDB();
    const result = retryOnFailure(() => db.adminCommand({replSetGetStatus: 1}));
    print("Done executing replSetGetStatus. result:", JSON.stringify(result));
    assert(
        result.errmsg === "replSetGetStatus is not supported through mongos" ||
            (result.ok &&
                result.myState !== undefined &&
                result.myState !== 4 &&
                result.myState !== 6 &&
                result.myState !== 8),
        `replSetGetStatus returned an unexpected result: ${JSON.stringify(result)}`,
    );
}
