var ClusteredCappedUtils = class {
    // Validate TTL-based deletion on a clustered, capped collection.
    static testClusteredCappedCollectionWithTTL(db, collName, clusterKeyField) {
        jsTest.log("Validating TTL operation on capped clustered collection");

        // Set expireAfterSeconds to a day to safely test that only expired documents are deleted.
        const expireAfterSeconds = 60 * 60 * 24;

        const clusterKey = {[clusterKeyField]: 1};
        const coll = db[collName];
        const now = new Date();
        const batchSize = 10;
        const clusterKeyFieldName = Object.keys(clusterKey)[0];

        coll.drop();

        assert.commandWorked(db.createCollection(
            coll.getName(),
            {clusteredIndex: {key: clusterKey, unique: true}, capped: true, expireAfterSeconds}));

        let docs = [];
        for (let i = batchSize; i; i--) {
            const tenTimesExpiredMs = 10 * expireAfterSeconds * 1000;
            const pastDate = new Date(now - tenTimesExpiredMs - i);
            docs.push({
                [clusterKeyFieldName]: pastDate,
                info: "expired",
            });
        }
        assert.commandWorked(coll.insertMany(docs, {ordered: true}));

        docs = [];
        for (let i = batchSize; i; i--) {
            const recentDate = new Date(now - i);
            docs.push({
                [clusterKeyFieldName]: recentDate,
                info: "unexpired",
            });
        }
        assert.commandWorked(coll.insertMany(docs, {ordered: true}));

        ClusteredCollectionUtil.waitForTTL(db);

        // Only the recent documents survived.
        assert.eq(coll.find().itcount(), batchSize);

        coll.drop();
    }

    static testClusteredTailableCursorCreation(db, collName, clusterKey, isReplicated) {
        jsTest.log(
            "Validating tailable cursor creation on capped clustered collection (isReplicated: " +
            isReplicated + ")");

        assert.commandWorked(db.createCollection(collName, {
            clusteredIndex: {key: {[clusterKey]: 1}, unique: true},
            capped: true,
            expireAfterSeconds: 10
        }));
        if (isReplicated) {
            // Must tail with read concern majority.
            assert.commandFailedWithCode(
                db.runCommand({find: collName, tailable: true, readConcern: {level: "local"}}),
                6049203);
            assert.commandFailedWithCode(
                db.runCommand({find: collName, tailable: true, readConcern: {level: "available"}}),
                6049203);
            assert.commandWorked(
                db.runCommand({find: collName, tailable: true, readConcern: {level: "majority"}}));
        } else {
            assert.commandWorked(
                db.runCommand({find: collName, tailable: true, readConcern: {level: "local"}}));
            assert.commandWorked(
                db.runCommand({find: collName, tailable: true, readConcern: {level: "available"}}));
        }
        db.getCollection(collName).drop();
    }

    // Validate tailable cursor operation along with TTL deletion.
    static testClusteredTailableCursorWithTTL(db, collName, clusterKey, isReplicated, awaitData) {
        jsTest.log(
            "Validating tailable cursor operation on capped clustered collection (isReplicated: " +
            isReplicated + ", awaitData: " + awaitData + ")");

        assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: false}));

        const oneDayInSeconds = 60 * 60 * 24;
        const oneHourInMilliseconds = 60 * 60 * 1000;
        const nineDaysInMilliseconds = 9 * oneDayInSeconds * 1000;
        const tenDaysInMilliseconds = 10 * oneDayInSeconds * 1000;

        const now = new Date();
        const oneHourAgo = new Date(now - oneHourInMilliseconds);
        const nineDaysAgo = new Date(now - nineDaysInMilliseconds);
        const tenDaysAgo = new Date(now - tenDaysInMilliseconds);

        // Create a clustered capped collection, and insert two old documents subject to imminent
        // TTL deletion, and two recent document which survive TTL deletion.

        assert.commandWorked(db.createCollection(collName, {
            clusteredIndex: {key: {[clusterKey]: 1}, unique: true},
            capped: true,
            expireAfterSeconds: oneDayInSeconds
        }));
        assert.commandWorked(
            db.getCollection(collName).insertOne({[clusterKey]: tenDaysAgo, info: "10 days ago"}));
        assert.commandWorked(
            db.getCollection(collName).insertOne({[clusterKey]: nineDaysAgo, info: "9 days ago"}));
        assert.commandWorked(db.getCollection(collName).insertOne(
            {[clusterKey]: oneHourAgo, info: "1 hour ago - surviving"}));
        assert.commandWorked(
            db.getCollection(collName).insertOne({[clusterKey]: now, info: "now - surviving"}));

        // Tail just past the first two documents, so the cursor can survive the upcoming TTL
        // deletion.
        let tailCommand = {find: collName, batchSize: 1, tailable: true, awaitData: awaitData};
        if (isReplicated) {
            tailCommand['readConcern'] = {level: "majority"};
        } else {
            tailCommand['readConcern'] = {level: "local"};
        }
        const tailable = db.runCommand(tailCommand);
        assert.commandWorked(tailable);
        const cursorId = tailable.cursor.id;
        assert(!bsonBinaryEqual({cursorId: cursorId}, {cursorId: NumberLong(0)}));
        assert.eq("10 days ago", tailable.cursor.firstBatch[0].info);

        {
            let getMore = db.runCommand({getMore: cursorId, collection: collName, batchSize: 1});
            assert.commandWorked(getMore);
            assert.eq("9 days ago", getMore.cursor.nextBatch[0].info);
        }
        {
            let getMore = db.runCommand({getMore: cursorId, collection: collName, batchSize: 1});
            assert.commandWorked(getMore);
            assert.eq("1 hour ago - surviving", getMore.cursor.nextBatch[0].info);
        }
        assert.eq(4, db.getCollection(collName).find().itcount());

        // TTL delete the two old documents.

        assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: true}));
        ClusteredCollectionUtil.waitForTTL(db);
        assert.eq(2, db.getCollection(collName).find().itcount());

        // Confirm that the tailable getMore can resume from where it was, since the document the
        // cursor is positioned on hasn't been TTL-removed.

        {
            let getMore = db.runCommand({getMore: cursorId, collection: collName, batchSize: 1});
            assert.commandWorked(getMore);
            assert.eq("now - surviving", getMore.cursor.nextBatch[0].info);
        }
        {
            let getMore = db.runCommand({getMore: cursorId, collection: collName});
            assert.commandWorked(getMore);
            assert.eq(0, getMore.cursor.nextBatch.length);
        }
        db.getCollection(collName).drop();
    }

    // Validate tailable cursor not keeping up with TTL deletion - CappedPositionLost.
    static testClusteredTailableCursorCappedPositionLostWithTTL(
        db, collName, clusterKey, isReplicated, awaitData) {
        jsTest.log(
            "Validating tailable cursor falling behind with TTL deletion on capped clustered collection (isReplicated: " +
            isReplicated + ", awaitData: " + awaitData + ")");

        assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: false}));

        const oneDayInSeconds = 60 * 60 * 24;
        const nineDaysInMilliseconds = 9 * oneDayInSeconds * 1000;
        const tenDaysInMilliseconds = 10 * oneDayInSeconds * 1000;

        const tenDaysAgo = new Date(new Date() - tenDaysInMilliseconds);
        const nineDaysAgo = new Date(new Date() - nineDaysInMilliseconds);
        const today = new Date();

        // Create a clustered capped collection, and insert two old documents subject to imminent
        // TTL deletion, and a recent document.

        assert.commandWorked(db.createCollection(collName, {
            clusteredIndex: {key: {[clusterKey]: 1}, unique: true},
            capped: true,
            expireAfterSeconds: oneDayInSeconds
        }));
        assert.commandWorked(
            db.getCollection(collName).insertOne({[clusterKey]: tenDaysAgo, info: "10 days ago"}));
        assert.commandWorked(
            db.getCollection(collName).insertOne({[clusterKey]: nineDaysAgo, info: "9 days ago"}));
        assert.commandWorked(
            db.getCollection(collName).insertOne({[clusterKey]: today, info: "today - surviving"}));

        // Tail up to and including the first document, before it gets TTL reaped.
        let tailCommand = {find: collName, batchSize: 1, tailable: true, awaitData: awaitData};
        if (isReplicated) {
            tailCommand['readConcern'] = {level: "majority"};
        } else {
            tailCommand['readConcern'] = {level: "local"};
        }
        const tailable = db.runCommand(tailCommand);
        assert.commandWorked(tailable);
        const cursorId = tailable.cursor.id;
        assert(!bsonBinaryEqual({cursorId: cursorId}, {cursorId: NumberLong(0)}));
        assert.eq("10 days ago", tailable.cursor.firstBatch[0].info);

        assert.eq(3, db.getCollection(collName).find().itcount());

        // TTL delete the two old documents, while the tailable cursor is still on the first one.

        assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: true}));
        ClusteredCollectionUtil.waitForTTL(db);
        assert.eq(1, db.getCollection(collName).find().itcount());

        // Confirm that the tailable cursor returns CappedPositionLost, as the document it was
        // pointing to has been TTL-deleted.

        let getMore = db.runCommand({getMore: cursorId, collection: collName, batchSize: 1});
        assert.commandFailedWithCode(getMore, 136);
        assert.eq(getMore.codeName, "CappedPositionLost");

        db.getCollection(collName).drop();
    }

    // Validate that by design, a cursor on a clustered capped collection can miss documents if they
    // are not inserted in cluster key order.
    static testClusteredTailableCursorOutOfOrderInsertion(
        db, collName, clusterKey, isReplicated, awaitData) {
        jsTest.log(
            "Validating clustered tailable cursor with out-of-order insertions (isReplicated: " +
            isReplicated + ", awaitData: " + awaitData + ")");

        assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: false}));

        const oneDayInSeconds = 60 * 60 * 24;
        const oneMinuteAgo = new Date(new Date() - 60 * 1000);
        const now = new Date();

        // Create a clustered capped collection and insert a document with a cluster key value equal
        // to now.
        assert.commandWorked(db.createCollection(collName, {
            clusteredIndex: {key: {[clusterKey]: 1}, unique: true},
            capped: true,
            expireAfterSeconds: oneDayInSeconds
        }));
        assert.commandWorked(
            db.getCollection(collName).insertOne({[clusterKey]: now, info: "now"}));

        // Create a tailable cursor and fetch the document inserted.

        let tailCommand = {find: collName, batchSize: 1, tailable: true, awaitData: awaitData};
        if (isReplicated) {
            tailCommand['readConcern'] = {level: "majority"};
        } else {
            tailCommand['readConcern'] = {level: "local"};
        }
        const tailable = db.runCommand(tailCommand);
        assert.commandWorked(tailable);
        const cursorId = tailable.cursor.id;
        assert(!bsonBinaryEqual({cursorId: cursorId}, {cursorId: NumberLong(0)}));
        assert.eq("now", tailable.cursor.firstBatch[0].info);

        // Now insert a document with a cluster key value equal to a minute ago, and verify
        // that the tailable cursor is unable to fetch it.
        assert.commandWorked(db.getCollection(collName).insertOne(
            {[clusterKey]: oneMinuteAgo, info: "1 minute ago"}));

        const getMore = db.runCommand({getMore: cursorId, collection: collName, batchSize: 1});
        assert.commandWorked(getMore);
        assert.eq(0, getMore.cursor.nextBatch.length);

        db.getCollection(collName).drop();
    }

    static testClusteredReplicatedTTLDeletion(db, collName) {
        jsTest.log("Validating replication of TTL deletes on capped clustered collection");

        assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: false}));

        const oneDayInSeconds = 60 * 60 * 24;
        const tenDaysInMilliseconds = 10 * oneDayInSeconds * 1000;
        const tenDaysAgo = new Date(new Date() - tenDaysInMilliseconds);

        // Create clustered capped collection and insert a soon-to-be-expired document.
        assert.commandWorked(db.createCollection(collName, {
            clusteredIndex: {key: {_id: 1}, unique: true},
            capped: true,
            expireAfterSeconds: oneDayInSeconds
        }));
        assert.commandWorked(
            db.getCollection(collName).insertOne({_id: tenDaysAgo, info: "10 days ago"}));

        // Expire the document.
        assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: true}));
        ClusteredCollectionUtil.waitForTTL(db);
        assert.eq(0, db.getCollection(collName).find().itcount());

        // The TTL deletion has been replicated to the oplog.
        const isBatched = assert.commandWorked(db.adminCommand(
            {getParameter: 1, "ttlMonitorBatchDeletes": 1}))["ttlMonitorBatchDeletes"];
        const ns = db.getName() + "." + collName;

        const featureFlagBatchMultiDeletes = assert.commandWorked(db.adminCommand({
            getParameter: 1,
            "featureFlagBatchMultiDeletes": 1
        }))["featureFlagBatchMultiDeletes"]["value"];

        if (featureFlagBatchMultiDeletes && isBatched) {
            assert.eq(1,
                      db.getSiblingDB("local")
                          .oplog.rs
                          .find({
                              op: "c",
                              ns: "admin.$cmd",
                              "o.applyOps": {$elemMatch: {op: "d", ns: ns, "o._id": tenDaysAgo}}
                          })
                          .itcount());
        } else {
            assert.eq(1,
                      db.getSiblingDB("local")
                          .oplog.rs.find({op: "d", ns: ns, "o._id": tenDaysAgo})
                          .itcount());
        }

        db.getCollection(collName).drop();
    }
};
