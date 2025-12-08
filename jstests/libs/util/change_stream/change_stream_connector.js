/**
 * Connector class for managing change stream events and test coordination.
 * Each instance uses a collection in the control database to store its change events.
 * Test completion is coordinated via a notification collection.
 */
class Connector {
    /**
     * Read all change events for a specific instance.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} controlDbName - Name of the control database for test coordination.
     * @param {string} instanceName - Name of the test instance.
     * @returns {Array} Array of change event records, sorted by _id.
     */
    static readAllChangeEvents(conn, controlDbName, instanceName) {
        const db = conn.getDB(controlDbName);
        const coll = db.getCollection(instanceName);
        return coll
            .find()
            .sort({changeEvent: {_id: 1}})
            .toArray();
    }

    /**
     * Write a change event record for a specific instance.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} controlDbName - Name of the control database for test coordination.
     * @param {string} instanceName - Name of the test instance.
     * @param {Object} record - Change event record to write.
     */
    static writeChangeEvent(conn, controlDbName, instanceName, record) {
        const db = conn.getDB(controlDbName);
        const coll = db.getCollection(instanceName);
        assert.commandWorked(coll.insertOne(record));
    }

    /**
     * Check if a test instance has completed.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} controlDbName - Name of the control database for test coordination.
     * @param {string} notificationCollName - Name of the notification collection for test completion.
     * @param {string} instanceName - Name of the test instance.
     * @returns {boolean} True if the instance is done, false otherwise.
     */
    static isDone(conn, controlDbName, notificationCollName, instanceName) {
        const db = conn.getDB(controlDbName);
        const coll = db.getCollection(notificationCollName);
        const doc = coll.findOne({_id: instanceName});
        return !!doc && doc.done === true;
    }

    /**
     * Wait for a test instance to complete.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} controlDbName - Name of the control database for test coordination.
     * @param {string} notificationCollName - Name of the notification collection for test completion.
     * @param {string} instanceName - Name of the test instance.
     */
    static waitForDone(conn, controlDbName, notificationCollName, instanceName) {
        assert.soonNoExcept(() => Connector.isDone(conn, controlDbName, notificationCollName, instanceName));
    }

    /**
     * Mark a test instance as done.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} controlDbName - Name of the control database for test coordination.
     * @param {string} notificationCollName - Name of the notification collection for test completion.
     * @param {string} instanceName - Name of the test instance.
     */
    static notifyDone(conn, controlDbName, notificationCollName, instanceName) {
        const db = conn.getDB(controlDbName);
        const coll = db.getCollection(notificationCollName);
        assert.commandWorked(coll.updateOne({_id: instanceName}, {$set: {done: true}}, {upsert: true}));
    }

    /**
     * Clean up the backing collection for a test instance.
     * This should be called after test completion to remove the collection storing change events.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} controlDbName - Name of the control database for test coordination.
     * @param {string} instanceName - Name of the test instance.
     */
    static cleanupChangeEvents(conn, controlDbName, instanceName) {
        const db = conn.getDB(controlDbName);
        const coll = db.getCollection(instanceName);
        coll.drop();
    }
}

export {Connector};
