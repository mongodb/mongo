/**
 * Concurrently runs 'replSetResizeOplog' with inserts and oplog scans and verifies that our oplog
 * scans wait for oplog visibility correctly.
 *
 * @tags: [requires_replication]
 */

export const $config = (function() {
    var states = (function() {
        function resizeOplog(db, collName) {
            const oplogSizeBytes = (20 + Math.floor(50 * Math.random())) * 1024 * 1024;
            jsTestLog("Setting " + tojson(oplogSizeBytes));
            assert.commandWorked(db.adminCommand({replSetResizeOplog: 1, size: oplogSizeBytes}));
        }

        function insertDocs(db, collName) {
            const numDocs = Math.floor(10 * Math.random());
            let docs = [];
            for (let i = 0; i < numDocs; i++) {
                docs.push({a: i});
            }

            assert.commandWorked(db[collName].insertMany(docs));
        }

        function scanOplog(db, collName) {
            try {
                assert.gte(db.getSiblingDB("local")["oplog.rs"].find().limit(20).itcount(), 0);
            } catch (e) {
                if (e.code == ErrorCodes.CappedPositionLost) {
                    return;
                } else {
                    throw e;
                }
            }
        }

        return {
            resizeOplog: resizeOplog,
            insertDocs: insertDocs,
            scanOplog: scanOplog,
        };
    })();

    var transitions = {
        resizeOplog: {resizeOplog: 0.1, insertDocs: 0.2, scanOplog: 0.7},
        insertDocs: {resizeOplog: 0.1, insertDocs: 0.2, scanOplog: 0.7},
        scanOplog: {resizeOplog: 0.1, insertDocs: 0.2, scanOplog: 0.7},
    };

    return {
        threadCount: 4,
        iterations: 100,
        startState: 'insertDocs',
        data: {},
        states: states,
        transitions: transitions,
    };
})();
