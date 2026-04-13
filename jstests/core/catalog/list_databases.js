/**
 * Tests for the listDatabases command.
 *
 * @tags: [
 *    # Uses $where operator.
 *    requires_scripting,
 * ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

// TODO (SERVER-60746) remove this once the issue is fixed.
TestData.pinToSingleMongos = true;

// Detect if collections are implicitly sharded
const isImplicitlyShardedCollection = typeof globalThis.ImplicitlyShardAccessCollSettings !== "undefined";

// Given the output from the listDatabases command, ensures that the total size reported is the
// sum of the individual db sizes.
function verifySizeSum(listDatabasesOut) {
    assert(listDatabasesOut.hasOwnProperty("databases"));
    const dbList = listDatabasesOut.databases;
    let sizeSum = 0;
    for (let i = 0; i < dbList.length; i++) {
        sizeSum += dbList[i].sizeOnDisk;
    }
    assert.eq(sizeSum, listDatabasesOut.totalSize);
}

function verifyNameOnly(listDatabasesOut) {
    // Delete extra meta info only returned by shardsvrs.
    delete listDatabasesOut.lastCommittedOpTime;

    for (let field in listDatabasesOut) {
        assert(
            ["databases", "nameOnly", "ok", "operationTime", "$clusterTime"].some((f) => f == field),
            "unexpected field " + field,
        );
    }
    listDatabasesOut.databases.forEach((database) => {
        for (let field in database) {
            assert.eq(field, "name", "expected name only");
        }
    });
}

describe("listDatabases command", function () {
    before(function () {
        db.getSiblingDB("jstest_list_databases_foo").dropDatabase();
        db.getSiblingDB("jstest_list_databases_bar").dropDatabase();
        db.getSiblingDB("jstest_list_databases_baz").dropDatabase();
        db.getSiblingDB("jstest_list_databases_zap").dropDatabase();
        db.getSiblingDB("jstest_list_databases_foo").coll.insert({});
        db.getSiblingDB("jstest_list_databases_bar").coll.insert({});
        db.getSiblingDB("jstest_list_databases_baz").coll.insert({});
        db.getSiblingDB("jstest_list_databases_zap").coll.insert({});
    });

    after(function () {
        db.getSiblingDB("jstest_list_databases_foo").dropDatabase();
        db.getSiblingDB("jstest_list_databases_bar").dropDatabase();
        db.getSiblingDB("jstest_list_databases_baz").dropDatabase();
        db.getSiblingDB("jstest_list_databases_zap").dropDatabase();
    });

    it("should list all test databases", function () {
        let cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: /jstest_list_databases/}}));
        assert.eq(4, cmdRes.databases.length);
        verifySizeSum(cmdRes);
    });

    it("should only list databases starting with a particular prefix", function () {
        let cmdRes = assert.commandWorked(
            db.adminCommand({listDatabases: 1, filter: {name: /^jstest_list_databases_ba/}}),
        );
        assert.eq(2, cmdRes.databases.length);
        verifySizeSum(cmdRes);
    });

    it("should return only the admin database", function () {
        let cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: "admin"}}));
        assert.eq(1, cmdRes.databases.length);
        verifySizeSum(cmdRes);
    });

    it("should return only the names", function () {
        let cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, nameOnly: true}));
        assert.lte(4, cmdRes.databases.length, tojson(cmdRes));
        verifyNameOnly(cmdRes);
    });

    it("should return only the name of the zap database", function () {
        let cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, nameOnly: true, filter: {name: /zap/}}));
        assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
        verifyNameOnly(cmdRes);
    });

    it("should support $expr in filter", function () {
        let cmdRes = assert.commandWorked(
            db.adminCommand({listDatabases: 1, filter: {$expr: {$eq: ["$name", "jstest_list_databases_zap"]}}}),
        );
        assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
        assert.eq("jstest_list_databases_zap", cmdRes.databases[0].name, tojson(cmdRes));
    });

    it("should fail $expr with an unbound variable in filter", function () {
        assert.commandFailed(db.adminCommand({listDatabases: 1, filter: {$expr: {$eq: ["$name", "$$unbound"]}}}));
    });

    it("should fail $expr with a filter that throws at runtime", function () {
        assert.commandFailed(db.adminCommand({listDatabases: 1, filter: {$expr: {$abs: "$name"}}}));
    });

    it("should not allow extensions in filters", function () {
        assert.commandFailed(db.adminCommand({listDatabases: 1, filter: {$text: {$search: "str"}}}));
        assert.commandFailed(
            db.adminCommand({
                listDatabases: 1,
                filter: {
                    $where: function () {
                        return true;
                    },
                },
            }),
        );
        assert.commandFailed(
            db.adminCommand({
                listDatabases: 1,
                filter: {a: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}},
            }),
        );
    });
});

describe("listDatabases shows databases created implicitly via different commands", function () {
    const prefix = jsTestName();
    const testDBs = {
        insert1: prefix + "_via_insert_1",
        insert2: prefix + "_via_insert_2",
        insert3: prefix + "_via_insert_3",
        update: prefix + "_via_update",
        findAndModify: prefix + "_via_findAndModify",
        index: prefix + "_via_index",
        rename: prefix + "_via_rename",
        rename2: prefix + "_via_rename_2",
        createCollection: prefix + "_via_createCollection",
        agg: prefix + "_via_agg",
        aggOut: prefix + "_via_agg_out",
        aggMerge: prefix + "_via_agg_merge",
        drop: prefix + "_drop",
        dropDatabase: prefix + "_drop_database",
    };

    before(function () {
        for (const name of Object.values(testDBs)) {
            db.getSiblingDB(name).dropDatabase();
        }
    });

    after(function () {
        for (const name of Object.values(testDBs)) {
            db.getSiblingDB(name).dropDatabase();
        }
    });

    it("should list a database created via insert", function () {
        assert.commandWorked(db.getSiblingDB(testDBs.insert1).coll.insertOne({x: 1}));
        assert.commandWorked(db.getSiblingDB(testDBs.insert2).coll.insertMany([{x: 1}, {x: 2}]));
        assert.commandWorked(db.getSiblingDB(testDBs.insert3).coll.insert({x: 1}));

        let cmdRes = assert.commandWorked(
            db.adminCommand({listDatabases: 1, filter: {name: new RegExp(prefix + "_via_insert")}}),
        );
        assert.eq(3, cmdRes.databases.length, tojson(cmdRes));
        const dbNames = cmdRes.databases.map((d) => d.name);
        assert(dbNames.includes(testDBs.insert1));
        assert(dbNames.includes(testDBs.insert2));
        assert(dbNames.includes(testDBs.insert3));
    });

    it("should list a database created via update with upsert", function () {
        assert.commandWorked(db.getSiblingDB(testDBs.update).coll.updateOne({x: 1}, {$set: {x: 1}}, {upsert: true}));

        let cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: testDBs.update}}));
        assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
        assert.eq(testDBs.update, cmdRes.databases[0].name);
    });

    it("should list a database created via findAndModify with upsert", function () {
        assert.commandWorked(
            db
                .getSiblingDB(testDBs.findAndModify)
                .runCommand({findAndModify: "coll", query: {x: 1}, update: {$set: {x: 1}}, upsert: true}),
        );

        let cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: testDBs.findAndModify}}));
        assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
        assert.eq(testDBs.findAndModify, cmdRes.databases[0].name);
    });

    it("should list a database created via create index", function () {
        assert.commandWorked(
            db.getSiblingDB(testDBs.index).runCommand({
                createIndexes: "coll",
                indexes: [{key: {x: 1}, name: "x_1"}],
            }),
        );

        let cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: testDBs.index}}));
        assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
        assert.eq(testDBs.index, cmdRes.databases[0].name);
    });

    it("should list a database created via renaming", function () {
        if (FixtureHelpers.isMongos(db)) {
            // Cross-database renameCollection through mongos requires the destination database
            // to already exist (the router must know where to route it). Since this test
            // verifies that the database is created implicitly by the rename, it cannot work
            // through mongos. Additionally, sharding passthroughs may implicitly shard the
            // source collection, which also prevents cross-database renames.
            jsTest.log.info("Skipping: cross-database rename through mongos is not supported");
            return;
        }
        if (TestData.isAnalyzingShardKey || isImplicitlyShardedCollection) {
            // Several passthrough suites (e.g. analyze_shard_key_jscore_passthrough,
            // multi_stmt_txn_jscore_passthrough_with_migration) override
            // DB.prototype.getCollection to implicitly shard every collection on first access.
            // By the time we call renameCollection, the source collection is already sharded,
            // and cross-database renames are not supported for sharded collections.
            jsTest.log.info("Skipping: cross-database rename is not supported for sharded collections");
            return;
        }
        if (
            TestData.runningWithShardStepdowns ||
            TestData.runningWithStepdowns ||
            (TestData.networkErrorAndTxnOverrideConfig &&
                TestData.networkErrorAndTxnOverrideConfig.retryOnNetworkErrors)
        ) {
            // renameCollection is not idempotent: if a stepdown occurs after the rename
            // succeeds, the automatic retry fails with NamespaceNotFound because the source
            // collection no longer exists.
            jsTest.log.info("Skipping: renameCollection is not idempotent and cannot be retried after stepdown");
            return;
        }
        assert.commandWorked(db.getSiblingDB(testDBs.rename).coll.insertOne({x: 1}));
        assert.commandWorked(
            db.adminCommand({
                renameCollection: testDBs.rename + ".coll",
                to: testDBs.rename2 + ".coll",
            }),
        );

        let cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: testDBs.rename2}}));
        assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
        assert.eq(testDBs.rename2, cmdRes.databases[0].name);
    });

    it("should list a database created via createCollection", function () {
        assert.commandWorked(db.getSiblingDB(testDBs.createCollection).createCollection("coll"));

        let cmdRes = assert.commandWorked(
            db.adminCommand({listDatabases: 1, filter: {name: testDBs.createCollection}}),
        );
        assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
        assert.eq(testDBs.createCollection, cmdRes.databases[0].name);
    });

    it("should list a database created via aggregations", function () {
        if (
            TestData.runningWithShardStepdowns ||
            TestData.runningWithStepdowns ||
            (TestData.networkErrorAndTxnOverrideConfig &&
                TestData.networkErrorAndTxnOverrideConfig.retryOnNetworkErrors)
        ) {
            jsTest.log.info("Skipping aggregation test because $out and $merge are not retryable.");
            return;
        }

        const testDBAgg = db.getSiblingDB(testDBs.agg);

        assert.commandWorked(testDBAgg.coll.insertOne({x: 1}));

        testDBAgg.coll.aggregate([{$out: {db: testDBs.aggOut, coll: "coll"}}]);

        if (FixtureHelpers.isMongos(testDBAgg)) {
            // The $merge stage, unlike $out, does not implicitly create the target database on a sharded cluster
            assert.commandWorked(db.getSiblingDB(testDBs.aggMerge).createCollection("coll"));
        }
        testDBAgg.coll.aggregate([{$merge: {into: {db: testDBs.aggMerge, coll: "coll"}}}]);

        let cmdRes = assert.commandWorked(
            db.adminCommand({listDatabases: 1, filter: {name: new RegExp(prefix + "_via_agg")}}),
        );
        assert.eq(3, cmdRes.databases.length, tojson(cmdRes));
        const dbNames = cmdRes.databases.map((d) => d.name);
        assert(dbNames.includes(testDBs.agg));
        assert(dbNames.includes(testDBs.aggOut));
        assert(dbNames.includes(testDBs.aggMerge));
    });

    function dropCollectionTestCase(db, dbName) {
        let testDB = db.getSiblingDB(dbName);
        assert.commandWorked(testDB.coll.insertOne({x: 1}));
        let cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: dbName}}));
        assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
        assert(testDB.coll.drop());

        const binVersion = assert.commandWorked(
            db.adminCommand({
                serverStatus: 1,
            }),
        ).version;

        const is90orAbove = MongoRunner.compareBinVersions(binVersion, "9.0") >= 0;
        cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: dbName}}));
        if (is90orAbove && FixtureHelpers.isMongos(testDB)) {
            // Through mongos the database entry persists in config.databases after dropping
            // the last collection, so listDatabases still returns it marked as empty.
            assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
            assert(cmdRes.databases[0].empty, tojson(cmdRes));
        } else {
            assert.eq(0, cmdRes.databases.length, tojson(cmdRes));
        }

        assert.commandWorked(testDB.coll.insertOne({x: 1}));
        cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: dbName}}));
        assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
    }

    it("should not list a database if collection is dropped, unless the database is empty", function () {
        if (isImplicitlyShardedCollection) {
            const originalImplicitlyShardOnCreateCollectionOnly = TestData.implicitlyShardOnCreateCollectionOnly;
            try {
                TestData.implicitlyShardOnCreateCollectionOnly = true;
                dropCollectionTestCase(db, testDBs.drop);
            } finally {
                TestData.implicitlyShardOnCreateCollectionOnly = originalImplicitlyShardOnCreateCollectionOnly;
            }
        } else {
            dropCollectionTestCase(db, testDBs.drop);
        }
    });

    it("should not list a database if database is dropped", function () {
        assert.commandWorked(db.getSiblingDB(testDBs.dropDatabase).coll.insertOne({x: 1}));
        let cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: testDBs.dropDatabase}}));
        assert.eq(1, cmdRes.databases.length, tojson(cmdRes));

        assert.commandWorked(db.getSiblingDB(testDBs.dropDatabase).dropDatabase());
        if (!TestData.runningWithBalancer) {
            // When the balancer is running in the background, concurrent moveCollection operations
            // can cause the database to be recreated unexpectedly. Therefore, skip this assertion.
            cmdRes = assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: testDBs.dropDatabase}}));
            assert.eq(0, cmdRes.databases.length, tojson(cmdRes));
        }
    });
});
