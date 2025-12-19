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
     * Read all change events for a specific instance.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance (also used as collection name).
     * @returns {Array} Array of change event records, sorted by resume token.
     */
    static readAllChangeEvents(conn, instanceName) {
        const db = conn.getDB(Connector.controlDatabase);
        const coll = db.getCollection(instanceName);
        return coll.find().sort({"changeEvent._id": 1}).toArray();
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
    }

    /**
     * Check if a test instance has completed.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance.
     * @returns {boolean} True if the instance is done, false otherwise.
     */
    static isDone(conn, instanceName) {
        const db = conn.getDB(Connector.controlDatabase);
        const coll = db.getCollection(Connector.notificationsCollection);
        const doc = coll.findOne({_id: instanceName});
        return !!doc && doc.done === true;
    }

    /**
     * Wait for a test instance to complete.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance.
     */
    static waitForDone(conn, instanceName) {
        assert.soonNoExcept(() => Connector.isDone(conn, instanceName));
    }

    /**
     * Mark a test instance as done.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance.
     */
    static notifyDone(conn, instanceName) {
        const db = conn.getDB(Connector.controlDatabase);
        const coll = db.getCollection(Connector.notificationsCollection);
        assert.commandWorked(coll.updateOne({_id: instanceName}, {$set: {done: true}}, {upsert: true}));
    }

    /**
     * Clean up resources for a test instance.
     * Drops the change events collection and removes the notification document.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} instanceName - Name of the test instance.
     */
    static cleanup(conn, instanceName) {
        const db = conn.getDB(Connector.controlDatabase);
        // Drop the change events collection
        db.getCollection(instanceName).drop();
        // Remove the notification document
        db.getCollection(Connector.notificationsCollection).deleteOne({_id: instanceName});
    }
}

export {Connector};
