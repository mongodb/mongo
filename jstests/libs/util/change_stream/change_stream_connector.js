/**
 * Connector class for managing change stream events and test coordination.
 *
 * Uses two separate collections in the control database:
 * - Change events collection: named after instanceName, stores captured change events
 * - Notifications collection: shared collection for completion signals
 *
 * The notification mechanism (isDone/waitForDone/notifyDone) supports parallel execution
 * where Writer runs in a separate shell (e.g., startParallelShell) and Reader needs to
 * wait for Writer completion before reading captured events.
 */
class Connector {
    /**
     * Database name for test coordination.
     * Can be overridden for testing by setting Connector.controlDatabase = "customDbName".
     */
    static controlDatabase = "change_stream_test_control";

    /**
     * Collection name for storing completion notifications.
     */
    static notificationsCollection = "notifications";

    /**
     * Collection name for storing coordination locks between independent writers.
     */
    static locksCollection = "locks";

    /**
     * Read all change events for a specific instance.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance (also used as collection name).
     * @returns {Array} Array of change event records, sorted by resume token.
     */
    static readAllChangeEvents(conn, instanceName) {
        const db = conn.getDB(Connector.controlDatabase);
        const coll = db.getCollection(instanceName);
        return coll.find({}, {_id: 0}).sort({"changeEvent._id": 1}).toArray();
    }

    /**
     * Write a change event record for a specific instance.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance (also used as collection name).
     * @param {Object} record - Change event record to write.
     */
    static writeChangeEvent(conn, instanceName, record) {
        const db = conn.getDB(Connector.controlDatabase);
        const coll = db.getCollection(instanceName);
        assert.commandWorked(coll.insertOne(record));
        Connector.heartbeat(conn, instanceName);
    }

    /**
     * Read the notification document for a test instance.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance.
     * @returns {Object|null} The notification document, or null if not found.
     */
    static _getNotification(conn, instanceName) {
        const db = conn.getDB(Connector.controlDatabase);
        const coll = db.getCollection(Connector.notificationsCollection);
        return coll.findOne({_id: instanceName});
    }

    /**
     * Check if a test instance has completed.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance.
     * @returns {boolean} True if the instance is done, false otherwise.
     */
    static isDone(conn, instanceName) {
        const doc = Connector._getNotification(conn, instanceName);
        return !!doc && doc.done === true;
    }

    /**
     * Increment the heartbeat counter for a test instance.
     * Called by Writer/Reader after each unit of work (command executed / event read)
     * to signal progress. waitForDone() uses this to distinguish "slow but alive"
     * from "hung".
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance.
     */
    static heartbeat(conn, instanceName) {
        const db = conn.getDB(Connector.controlDatabase);
        const coll = db.getCollection(Connector.notificationsCollection);
        assert.commandWorked(
            coll.updateOne({_id: instanceName}, {$inc: {heartbeatSeq: 1}}, {upsert: true}),
        );
    }

    /**
     * Wait for a test instance to complete, using heartbeat-based progress detection.
     *
     * Loops using assert.soonNoExcept, each iteration bounded to one unit of work.
     * Each call waits for either "done" or "heartbeat counter incremented". If the
     * heartbeat increments, the loop resets and waits again. If neither happens
     * within the default assert.soon window (90s local / 10min Evergreen), the hang
     * analyzer fires — which is the correct behavior for a truly hung command.
     *
     * Total wait is unbounded as long as the background thread keeps making progress.
     *
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance.
     */
    static waitForDone(conn, instanceName) {
        let lastSeq = -1;
        let latestDoc = null;
        while (true) {
            assert.soonNoExcept(() => {
                latestDoc = Connector._getNotification(conn, instanceName);
                if (latestDoc && latestDoc.done === true) {
                    return true;
                }
                if (latestDoc && latestDoc.heartbeatSeq > lastSeq) {
                    return true;
                }
                return false;
            }, `Timed out waiting for ${instanceName} — no heartbeat progress (lastSeq=${lastSeq})`);
            if (latestDoc && latestDoc.done === true) {
                return;
            }
            lastSeq = latestDoc.heartbeatSeq;
        }
    }

    /**
     * Mark a test instance as done.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance.
     */
    static notifyDone(conn, instanceName) {
        const db = conn.getDB(Connector.controlDatabase);
        const coll = db.getCollection(Connector.notificationsCollection);
        assert.commandWorked(
            coll.updateOne({_id: instanceName}, {$set: {done: true}}, {upsert: true}),
        );
    }

    /**
     * Clean up resources for a test instance.
     * Drops the change events collection and removes the notification document.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance.
     */
    static cleanup(conn, instanceName) {
        const db = conn.getDB(Connector.controlDatabase);
        // Drop the change events collection.
        db.getCollection(instanceName).drop();
        // Remove the notification document.
        db.getCollection(Connector.notificationsCollection).deleteOne({_id: instanceName});
    }

    /**
     * Drop the entire control database. Faster than per-instance cleanup
     * when many inline readers (e.g. PrefixReadTestCase) created collections.
     * @param {Mongo} conn - MongoDB connection.
     */
    static cleanupAll(conn) {
        conn.getDB(Connector.controlDatabase).dropDatabase();
    }

    /**
     * Run `fn` while holding a lock scoped to `dbName`, acquired via atomic upsert against a
     * well-known collection (retrying until acquired) and released once `fn` returns or throws.
     * Used to serialize operations whose outcome depends on a resource shared across
     * otherwise-independent writers targeting the same database. Scoped per-database so writers
     * on unrelated databases don't serialize against each other.
     * @param {Mongo} conn    - MongoDB connection.
     * @param {string} dbName - Database whose primary-shard placement this lock protects.
     * @param {Function} fn   - Callback to run while holding the lock.
     * @returns {*} fn()'s return value.
     */
    static withDbLock(conn, dbName, fn) {
        const coll = conn.getDB(Connector.controlDatabase).getCollection(Connector.locksCollection);
        const holderId = new ObjectId().str;
        assert.soon(() => {
            try {
                const doc = coll.findAndModify({
                    query: {_id: dbName, holder: null},
                    update: {$set: {holder: holderId}},
                    upsert: true,
                    new: true,
                });
                return doc !== null && doc.holder === holderId;
            } catch (e) {
                // Someone else holds the lock, or we raced them for the initial insert.
                if (e.code === ErrorCodes.DuplicateKey) {
                    return false;
                }
                throw e;
            }
        }, `Timed out acquiring lock for database '${dbName}'`);
        try {
            return fn();
        } finally {
            assert.commandWorked(
                coll.updateOne({_id: dbName, holder: holderId}, {$set: {holder: null}}),
            );
        }
    }
}

export {Connector};
