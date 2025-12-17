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
                err.message === "read ECONNRESET" ||
                err.message === "read ETIMEDOUT" ||
                err.message === "Shutting down"
            ) {
                print(`Attempt ${retries} failed due to ${err.name}, retrying in 1 second...`);
                sleep(1000);
                continue;
            }
            if (
                Array.isArray(err.errorResponse?.errorLabels) &&
                err.errorResponse.errorLabels.includes("RetryableWriteError")
            ) {
                print(`Attempt ${retries} failed due to ${err.codeName}, retrying in 1 second...`);
                sleep(1000);
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

// Establishes a conenction
function getDB() {
    return retryOnFailure(() => new Mongo(MONGOD_URI).getDB("test"));
}

function find() {
    const db = getDB();
    print("executing find");
    for (var i = 0; i < 100; i++) {
        const val = Math.floor(Math.random() * 10000) + 1;
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
    for (var i = 0; i < 100; i++) {
        const val = Math.floor(Math.random() * 10000) + 1;
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
    assert(EJSON.stringify(snapshotReadResult.cursor.firstBatch) == EJSON.stringify([{_id, value: 1}]));
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
