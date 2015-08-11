/**
 * Test the upgrade/downgrade process for the last stable release <~~> the latest release
 * with the same storage engine (and options), via mongod binary swap.
 * New features/changes tested in 3.2:
 *  - Partial index
 *      This sets partialFilterExpression attribute for an index.
 *      After downgrade, mongod should fail to start (SERVER-17658).
 *  - Document validation
 *      This sets the validator attribute for a collection.
 *      After downgrade, this should be ignored.
 *  - Text index
 *      This sets the text index version to a new value.
 *      After downgrade, mongod should fail to start.
 *  - Geo index
 *      This sets the geo 2dsphere index version to a new value.
 *      After downgrade, mongod should fail to start (SERVER-19557).
*/

(function() {

    "use strict";

    function waitForPrimary(db, timeout, interval) {
        var timeStart = new Date().getTime();
        timeout = timeout || 60000;
        interval = interval || 500;
        while (!db.isMaster().ismaster) {
            sleep(interval);
            var timeNow = new Date().getTime();
            if (timeNow-timeStart > timeout) {
                return 0;
            }
        }
        return 1;
    }

    function init_basic(conn) {
        var testDB = conn.getDB('test');
        var coll = testDB.updown;
        var testName = this.name;
        var indexName = 'namedIndex';

        assert.commandWorked(testDB.createCollection("capped", {capped: true, size: 10000}),
            testName + ' basic createCollection');
        var capped = testDB.capped;

        // Insert documents
        for (var i = 0; i < this.data.numColl; i++) {
            if (i < this.data.numCapped) {
                assert.writeOK(capped.insert({x: i}),  testName + ' basic capped insert');
            }
            assert.writeOK(coll.insert({x: i, y: i, a: i}), testName + ' basic insert');
        }
        // Create a named index
        assert.commandWorked(coll.createIndex({x: 1}, {name: indexName}),
            testName + ' basic createIndex');
        this.data.indexNames[indexName] = 1;
        this.data.numIndex = coll.getIndexes().length;
    }

    function verify_basic(conn) {
        var testDB = conn.getDB('test');
        var coll = testDB.updown;
        var capped = testDB.capped;
        var indexes = coll.getIndexes();
        var testName = this.name;
        var indexNames = this.data.indexNames;

        assert.eq(this.data.numColl, coll.count(), testName + ' basic count');
        assert.eq(this.data.numIndex, indexes.length, testName + ' basic number of indexes');
        // Find the indexes
        indexes.forEach(function(index) {
            assert(index.name in indexNames, testName + ' basic find index');
        });
        assert.eq(this.data.numCapped, capped.count(), testName + ' basic capped count');
        assert(capped.isCapped(), testName + ' basic isCapped');
    }

    function init_ttl(conn) {
        var testDB = conn.getDB('test');
        var coll = testDB.ttl;
        var testName = this.name;
        var indexName = 'ttl';

        this.data.now = (new Date()).getTime();

        // Insert documents
        for (var i = 0; i < this.data.numColl; i++) {
            // past is i hours ago
            var past = new Date(this.data.now - (60000 * 60 * i));
            assert.writeOK(coll.insert({x: i, date: past}), testName + ' ttl insert');
        }
        // Ensure TTL monitor will not run
        assert.commandWorked(testDB.adminCommand({setParameter: 1, ttlMonitorEnabled: false}),
            testName + ' setParameter ttlMonitorEnabled');
        // Create a named ttl index, expire after 1 hour
        assert.commandWorked(coll.createIndex(
                                {date: 1},
                                {name: indexName, expireAfterSeconds: 60 * 60}),
            testName + ' ttl createIndex');
        this.data.indexNames[indexName] = 1;
        this.data.numIndex = coll.getIndexes().length;
    }

    function verify_ttl(conn) {
        var testDB = conn.getDB('test');
        var coll = testDB.ttl;
        var testName = this.name;

        // Wait until TTL monitor runs at least once
        var ttlPass = coll.getDB().serverStatus().metrics.ttl.passes;
        assert.soon(function() {
                        return coll.getDB().serverStatus().metrics.ttl.passes >= ttlPass + 1;
                    },
                    testName + " TTL monitor didn't run before timing out.");
        // All docs should be expired, except 1
        assert.eq(coll.count(), 1, testName + ' ttl count');
    }

    function init_ttl_partial(conn) {
        var testDB = conn.getDB('test');
        var coll = testDB.ttl;
        var testName = this.name;
        var indexName = 'ttl_partial';

        this.data.now = (new Date()).getTime();

        // Insert documents
        for (var i = 0; i < this.data.numColl; i++) {
            // past is i hours ago
            var past = new Date(this.data.now - (60000 * 60 * i));
            assert.writeOK(coll.insert({x: i, date: past}), testName + ' ttl_partial insert1');
            assert.writeOK(coll.insert({x: i, date: past, z: i}),
                testName + ' ttl_partial insert2');
        }

        // Ensure TTL monitor will not run
        assert.commandWorked(testDB.adminCommand({setParameter: 1, ttlMonitorEnabled: false}),
            testName + ' ttl_partial setParameter ttlMonitorEnabled');
        // Create a named ttl index, expire after 1 hour
        assert.commandWorked(coll.createIndex(
                                {date: 1},
                                {name: indexName, expireAfterSeconds: 60 * 60,
                                 partialFilterExpression: {z: {$exists: true}}}),
            testName + ' ttl_partial createIndex');
        this.data.indexNames[indexName] = 1;
        this.data.numIndex = coll.getIndexes().length;
    }

    // In 3.1.8? downgrade with partial index, mongod should fail to start, due to index
    // incompatibility
    // Note - this behavior is still not confirmed (see SERVER-17658)
    function verify_ttl_partial(conn) {
        var testDB = conn.getDB('test');
        var coll = testDB.ttl;
        var testName = this.name;

        // Wait until TTL monitor runs at least once
        var ttlPass = coll.getDB().serverStatus().metrics.ttl.passes;
        assert.soon(function() {
                        return coll.getDB().serverStatus().metrics.ttl.passes >= ttlPass + 1;
                    },
                    testName + " TTL monitor didn't run before timing out.");
        // All docs should be expired, except 2
        // Current behavior is all expired partial index docs are removed
        // Leaving 50 (non_partial) + 1 unexpired partial
        // assert.eq(coll.count(), 2, testName + ' ttl_partial count2');
        assert.eq(coll.count(), this.data.numColl + 1, testName + ' ttl_partial count');
    }

    function init_partial_index(conn) {
        var testDB = conn.getDB('test');
        var coll = testDB.partial;
        var testName = this.name;
        var indexName = 'partial_index';

        // Create a partial index
        assert.commandWorked(coll.createIndex(
                                {x: 1},
                                {name: indexName,
                                 partialFilterExpression: {a: {$lt: this.data.partial}}}),
            testName + ' partial_index createIndex');
        // Insert doc that will be indexed
        assert.writeOK(coll.insert({x: 1, a: this.data.partial-1}),
            testName + ' partial_index insert1');
        // In 3.2 insert doc that will not be indexed
        assert.writeOK(coll.insert({x: 2, a: this.data.partial}),
            testName + ' partial_index insert2');
        assert.eq(coll.validate().keysPerIndex["test.partial.$" + indexName],
            1,
            testName + ' partial_index validate');
        this.data.indexNames[indexName] = 1;
        this.data.numIndex = coll.getIndexes().length;
    }

    // In 3.1.8? downgrade with partial index, mongod should fail to start, due to index
    // incompatibility
    // Note - this behavior is still not confirmed (see SERVER-17658)
    // On upgrades, existing documents will be indexed. New documents will apply to filter.

    function verify_partial_index(conn) {
        var testDB = conn.getDB('test');
        var coll = testDB.partial;
        var testName = this.name;
        var indexName = 'partial_index';

        assert.eq(coll.validate().keysPerIndex["test.partial.$" + indexName],
            1,
            testName + ' partial_index validate1');
        // Insert doc that will be indexed
        assert.writeOK(coll.insert({x: 3, a: this.data.partial - 2}),
            testName + ' partial_index insert1');
        // In 3.2 insert doc that will not be indexed
        assert.writeOK(coll.insert({x: 4, a: this.data.partial + 1}),
            testName + ' partial_index insert2');
        assert.eq(coll.validate().keysPerIndex["test.partial.$" + indexName],
            3,
            testName + ' partial_index validate2');
        // Remove the documents with partial indexes
        assert.writeOK(coll.remove({x: 1, a: this.data.partial - 1}),
            testName + ' partial_index remove1');
        assert.writeOK(coll.remove({x: 2, a: this.data.partial}),
            testName + ' partial_index remove2');
        assert.writeOK(coll.remove({x: 3, a: this.data.partial - 2}),
            testName + ' partial_index remove3');
        assert.writeOK(coll.remove({x: 4, a: this.data.partial + 1}),
            testName + ' partial_index remove4');
        assert.eq(coll.validate().keysPerIndex["test.partial.$" + indexName], 0,
            testName + ' partial_index validate3');
    }

    function init_document_validation(conn) {
        var testDB = conn.getDB('test');
        var coll = testDB.docVal;
        var testName = this.name;

        // Create collection with document validator and insert 1 valid doc
        assert.commandWorked(
            testDB.createCollection("docVal", {validator: {a: {$exists: true}}}),
            testName + ' document_validation createCollection');
        assert.writeOK(coll.insert({a: 1}), testName + ' document_validation insert1');
        assert.writeError(coll.insert({b: 1}), testName + ' document_validation insert2');
        this.data.docVal = coll.count();
    }

    function verify_document_validation(conn) {
        var testDB = conn.getDB('test');
        var coll = testDB.docVal;
        var testName = this.name;

        // Verify documents are there
        assert.eq(coll.count(), this.data.docVal, testName + ' document_validation count1');
        // Insert, update and upsert documents
        assert.writeOK(coll.insert({a: 1}), testName + ' document_validation insert1');
        assert.writeOK(coll.insert({b: 1}), testName + ' document_validation insert2');
        assert.writeOK(coll.update({b: 1}, {c: 1}), testName + ' document_validation update');
        assert.writeOK(coll.update({b: 3}, {c: 1}, {upsert: true}),
            testName + ' document_validation upsert');
        // Verify documents are there
        assert.eq(coll.count(), this.data.docVal + 3, testName + ' document_validation count2');
    }

    function init_replication(conn){
        var testDB = conn.getDB('test');
        var testName = this.name;
        var rsconf = {_id: 'oplog', members: [ {_id: 0, host: 'localhost:' + conn.port}]};

        assert.commandWorked(testDB.adminCommand({replSetInitiate : rsconf}),
            testName + ' replSetInitiate');
        assert(waitForPrimary(testDB), testName + ' host did not become primary');
        // Insert 1 document
        assert.writeOK(testDB.repl.insert({a: 1}), testName + ' insert');
    }

    function verify_replication(conn){
        var testDB = conn.getDB('test');
        var testName = this.name;

        assert(waitForPrimary(testDB), testName + ' host did not become primary');
        assert.eq(conn.getDB("local").oplog.rs.findOne({ns: "test.repl"}).op,
            "i",
            testName + ' replication no oplog entry');
    }

    function init_fullTextSearch(conn) {
        var testDB = conn.getDB('test');
        var testName = this.name;
        var coll = testDB.fullTextSearch;
        var indexName = 'content';
        var indexSpec = {};

        indexSpec[indexName] = 'text';
        assert.commandWorked(coll.createIndex(indexSpec, {default_language: "spanish"}),
            testName + ' fullTextSearch createIndex');
        this.data.indexNames[indexName] = 1;
        this.data.numIndex = coll.getIndexes().length;
    }

    function verify_fullTextSearch(conn) {
        var testDB = conn.getDB('test');
        var testName = this.name;
        var coll = testDB.fullTextSearch;
        var indexName = 'content';

        assert.eq(1,
            coll.getIndexes().filter(function(z){ return z.name == indexName + "_text"; }).length,
            testName + ' fullTextSearch getIndexes');
    }

    function init_geo(conn) {
        var testDB = conn.getDB('test');
        var testName = this.name;
        var coll = testDB.geo;
        var indexName = 'geo';
        var indexSpec = {};

        indexSpec[indexName] = '2dsphere';
        assert.commandWorked(coll.createIndex(indexSpec),
            testName + ' geo createIndex');
        this.data.indexNames[indexName] = 1;
        this.data.numIndex = coll.getIndexes().length;
    }

    function verify_geo(conn) {
        var testDB = conn.getDB('test');
        var testName = this.name;
        var coll = testDB.geo;
        var indexName = 'geo';

        assert.eq(1,
            coll.getIndexes().filter(
                function(z){ return z.name == indexName + "_2dsphere"; }).length,
            testName + ' geo getIndexes');
    }


    // Upgrade/downgrade tests
    var tests = [
        // Upgrade with mmapv1
        {
            name: "Upgrade - mmapv1",
            fromBinVersion: "last-stable",
            toBinVersion: "latest",
            storageEngine: "mmapv1",
            data: {
                indexNames: {_id_: 1},
                numCapped: 10,
                numColl: 50,
            },
            init: [init_basic],
            verify: [verify_basic]
        },
        // Downgrade with mmapv1
        {
            name: "Downgrade - mmapv1",
            fromBinVersion: "latest",
            toBinVersion: "last-stable",
            storageEngine: "mmapv1",
            options: {setParameter: "ttlMonitorSleepSecs=3"},
            data: {
                indexNames: {_id_: 1},
                numCapped: 10,
                numColl: 50,
                partial: 5
            },
            init: [
                init_basic,
                init_ttl,
                init_partial_index,
                init_document_validation
            ],
            verify: [
                verify_basic,
                verify_ttl,
                verify_partial_index,
                verify_document_validation
            ]
        },
        // Downgrade with mmapv1 - ttl w/partial index
        // Enable this test when implemented (SERVER-17658)
        // {
        //     name: "Downgrade - mmapv1: ttl with partial index filter",
        //     fromBinVersion: "latest",
        //     toBinVersion: "last-stable",
        //     storageEngine: "mmapv1",
        //     options: {setParameter: "ttlMonitorSleepSecs=3"},
        //     data: {
        //         indexNames: {_id_: 1},
        //         numColl: 50,
        //         partial: 5
        //     },
        //     init: [init_ttl_partial],
        //     verify: [verify_ttl_partial]
        // },
        // Downgrade with mmapv1 - fullTextSearch index
        {
            name: "Downgrade - mmapv1: fullTextSearch index",
            fromBinVersion: "latest",
            toBinVersion: "last-stable",
            storageEngine: "mmapv1",
            data: {
                indexNames: {_id_: 1},
                // Uncomment this when implemented (SERVER-19557)
                // failedConn: true
            },
            init: [init_fullTextSearch],
            verify: [verify_fullTextSearch]
        },
        // Downgrade with mmapv1 - geo index
        {
            name: "Downgrade - mmapv1: geo index",
            fromBinVersion: "latest",
            toBinVersion: "last-stable",
            storageEngine: "mmapv1",
            data: {
                indexNames: {_id_: 1},
                failedConn: true
            },
            init: [init_geo],
            verify: [verify_geo]
        },
        // Downgrade with mmapv1 - oplog
        {
            name: "Downgrade - mmapv1: oplog",
            fromBinVersion: "latest",
            toBinVersion: "last-stable",
            storageEngine: "mmapv1",
            options: {replSet: "oplog"},
            data: {
                indexNames: {_id_: 1},
                numCapped: 10,
                numColl: 50,
            },
            init: [
                init_replication,
                init_basic
            ],
            verify: [
                verify_replication,
                verify_basic
            ],
        },
        // Upgrade with wiredTiger
        {
            name: "Upgrade - wiredTiger",
            fromBinVersion: "last-stable",
            toBinVersion: "latest",
            storageEngine: "wiredTiger",
            options: {setParameter: "ttlMonitorSleepSecs=3"},
            data: {
                indexNames: {_id_: 1},
                numCapped: 10,
                numColl: 50,
            },
            init: [
                init_basic,
                init_ttl,
                init_fullTextSearch,
                init_geo
            ],
            verify: [
                verify_basic,
                verify_ttl,
                verify_fullTextSearch,
                verify_geo
            ]
        },
        // Upgrade with wiredTiger LSM, nojournal
        {
            name: "Upgrade - wiredTiger: LSM, nojournal",
            fromBinVersion: "last-stable",
            toBinVersion: "latest",
            storageEngine: "wiredTiger",
            options: {
                wiredTigerCollectionConfigString: "type=lsm",
                wiredTigerIndexConfigString: "type=lsm",
                nojournal: "",
            },
            data: {
                indexNames: {_id_: 1},
                numCapped: 10,
                numColl: 50,
            },
            init: [init_basic],
            verify: [verify_basic]
        },
        // Downgrade with wiredTiger
        {
            name: "Downgrade - wiredTiger",
            fromBinVersion: "latest",
            toBinVersion: "last-stable",
            storageEngine: "wiredTiger",
            options: {replSet: "oplog"},
            data: {
                indexNames: {_id_: 1},
                numCapped: 10,
                numColl: 50,
                partial: 5,
                // Remove this next line when SERVER-19100 is fixed
                failedConn: true
            },
            init: [
                init_replication,
                init_basic,
                init_document_validation
            ],
            verify: [
                verify_replication,
                verify_basic,
                verify_document_validation
            ]
        },
        // Downgrade with wiredTiger - ttl w/partial index
        // Enable this test when implemented (SERVER-17658)
        // {
        //     name: "Downgrade - wiredTiger: ttl with partial index filter",
        //     fromBinVersion: "latest",
        //     toBinVersion: "last-stable",
        //     storageEngine: "wiredTiger",
        //     options: {setParameter: "ttlMonitorSleepSecs=3"},
        //     data: {
        //         indexNames: {_id_: 1},
        //         numCapped: 10,
        //         numColl: 50,
        //         partial: 5,
        //         failedConn: true
        //     },
        //     init: [init_ttl_partial],
        //     verify: [verify_ttl_partial]
        // },

        // Enable these 2 tests when SERVER-19100 is fixed
        // Downgrade with wiredTiger - fullTextSearch index
        // {
        //     name: "Downgrade - wiredTiger: fullTextSearch index",
        //     fromBinVersion: "latest",
        //     toBinVersion: "last-stable",
        //     storageEngine: "wiredTiger",
        //     data: {
        //         indexNames: {_id_: 1},
        //         failedConn: true
        //     },
        //     init: [init_fullTextSearch],
        //     verify: [verify_fullTextSearch]
        // },
        // // Downgrade with wiredTiger - geo index
        // {
        //     name: "Downgrade - wiredTiger: geo index",
        //     fromBinVersion: "latest",
        //     toBinVersion: "last-stable",
        //     storageEngine: "wiredTiger",
        //     data: {
        //         indexNames: {_id_: 1},
        //         failedConn: true
        //     },
        //     init: [init_geo],
        //     verify: [verify_geo]
        // },
    ];

    var conn;
    var expectedConn;

    tests.forEach(function(test) {

        jsTestLog(test.name);

        // Set mongod options
        var mongodOptions = {
            remember: true,
            cleanData: true,
            binVersion: test.fromBinVersion,
            storageEngine: test.storageEngine,
            smallfiles: ""
        };

        // Additional mongod options
        if (test.options) {
            for (var option in test.options) {
                mongodOptions[option] = test.options[option];
            }
        }

        // Start mongod
        var conn = MongoRunner.runMongod(mongodOptions);
        assert(conn, test.name + ' start');

        // Change writeMode to commands
        conn.forceWriteMode("commands");

        // Execute all init functions
        test.init.forEach(function(func) {
            func.call(test, conn);
        });

        // Stop existing mongod
        MongoRunner.stopMongod(conn);

        // Restart mongod under a different version
        mongodOptions.restart = conn;
        mongodOptions.binVersion = test.toBinVersion;
        mongodOptions.cleanData = false;
        conn = MongoRunner.runMongod(mongodOptions);
        if (test.data.failedConn) {
            // We are expecting a mongod failure
            expectedConn = null;
        } else {
            expectedConn = conn || true;
        }
        assert.eq(conn, expectedConn, test.name + ' restart');

        if (conn) {
            // Change writeMode to commands
            conn.forceWriteMode("commands");

            // Execute all verify functions
            test.verify.forEach(function(func) {
                func.call(test, conn);
            });

            // Test finished, stop mongod
            MongoRunner.stopMongod(conn);
        }
    });
}());