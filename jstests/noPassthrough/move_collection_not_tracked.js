/**
 * Verifies that namespaces which can't be moved/tracked do not get registered in the sharding
 * catalog when moveCollection is invoked on them.
 */
import {EncryptedClient} from "jstests/fle2/libs/encrypted_client_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const testCases = [
    {
        name: "local",
        tasks: [{nsToMove: "local.oplog.rs", expectedError: ErrorCodes.NamespaceNotFound}]
    },
    {name: "systemVersion", tasks: [{nsToMove: "admin.system.version"}]},
    {name: "systemKeys", tasks: [{nsToMove: "admin.system.keys"}]},
    {
        name: "systemRoles",
        setup: (st) => {
            assert.commandWorked(st.s.adminCommand({
                createRole: "testRole",
                privileges: [
                    {resource: {db: "roleDb1", collection: ""}, actions: ["find", "update"]},
                    {resource: {db: "roleDb2", collection: "foo"}, actions: ["insert", "remove"]}
                ],
                roles: []
            }));
        },
        tasks: [{nsToMove: "admin.system.roles"}]
    },
    {
        name: "systemUsers",
        setup: (st) => {
            assert.commandWorked(
                st.s.adminCommand({createUser: "testUser", pwd: "pwd", roles: []}));
        },
        tasks: [{nsToMove: "admin.system.users"}]
    },
    {
        name: "systemJS",
        setup: (st) => {
            const dbName = "testDbWithSystemJS";
            assert.commandWorked(
                st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
            assert.commandWorked(st.s.getDB(dbName).getCollection("system.js").insert({
                _id: "addOne",
                value: function(x) {
                    return x + 1;
                }
            }));
        },
        tasks: [{nsToMove: "testDbWithSystemJS.system.js"}]
    },
    {
        // TODO SERVER-93149 remove test case once timeseries will be moveable again
        name: "timeseries",
        setup: (st) => {
            const dbName = "testDbWithTimeseries";
            assert.commandWorked(
                st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
            assert.commandWorked(st.s.getDB(dbName).runCommand(
                {create: "testColl", timeseries: {timeField: "x", metaField: "y"}}));
        },
        tasks: [
            {nsToMove: "testDbWithTimeseries.system.views"},
            {
                nsToMove: "testDbWithTimeseries.testColl",
                shouldSkip: (conn) => FeatureFlagUtil.isEnabled(conn, "ReshardingForTimeseries")
            },
            {
                nsToMove: "testDbWithTimeseries.system.buckets.testColl",
                shouldSkip: (conn) => FeatureFlagUtil.isEnabled(conn, "ReshardingForTimeseries")
            }
        ],
    },
    {
        name: "views",
        setup: (st) => {
            const dbName = "testDbWithView";
            const collName = "testColl";
            const testDB = st.s.getDB(dbName);
            assert.commandWorked(
                st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
            assert.commandWorked(testDB.getCollection(collName).insert({x: 1}));
            assert.commandWorked(testDB.runCommand(
                {create: "testCollView", viewOn: collName, pipeline: [{$match: {}}]}));
        },
        tasks: [
            {nsToMove: "testDbWithView.system.views"},
            {nsToMove: "testDbWithView.testCollView", expectedError: ErrorCodes.NamespaceNotFound}
        ],
    },
    {
        // TODO SERVER-94425 remove test case once FLE state collections are moveable
        name: "fleState",
        shouldSkip: () => !buildInfo().modules.includes("enterprise"),
        setup: (st) => {
            const dbName = "testDbWithFLE";
            assert.commandWorked(
                st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
            const encryptedClient = new EncryptedClient(st.s, dbName);
            // Make sure that the internal esc and ecoc collections get created
            assert.commandWorked(encryptedClient.createEncryptionCollection("testColl", {
                encryptedFields: {
                    escCollection: "enxcol_.testColl.esc",
                    ecocCollection: "enxcol_.testColl.ecoc",
                    "fields": [
                        {"path": "x", "bsonType": "int", "queries": {"queryType": "equality"}},
                    ]
                }
            }));
            // Explicitly create dummy instances of the ecc and ecoc.compact collections
            encryptedClient.runEncryptionOperation(() => {
                assert.commandWorked(
                    encryptedClient.getDB().createCollection("enxcol_.testColl.ecc"));
                assert.commandWorked(
                    encryptedClient.getDB().createCollection("enxcol_.testColl.ecoc.compact"));
            });
        },
        tasks: [
            {nsToMove: "testDbWithFLE.enxcol_.testColl.esc"},
            {nsToMove: "testDbWithFLE.enxcol_.testColl.ecoc"},
            {nsToMove: "testDbWithFLE.enxcol_.testColl.ecoc.compact"},
            {nsToMove: "testDbWithFLE.enxcol_.testColl.ecc"}
        ]
    },
    {
        name: "systemResharding",
        setup: (st) => {
            const dbName = "testDbWithSystemResharding";
            const testDB = st.s.getDB(dbName);
            assert.commandWorked(
                st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
            // Creaty dummy instances of temporary reshrd collections
            assert.commandWorked(testDB.createCollection("system.resharding.testColl"));
            assert.commandWorked(
                testDB.createCollection("system.buckets.resharding.testTimeseriesColl",
                                        {timeseries: {timeField: "x", metaField: "y"}}));
        },
        tasks: [
            {nsToMove: "testDbWithSystemResharding.system.resharding.testColl"},
            {nsToMove: "testDbWithSystemResharding.system.buckets.resharding.testTimeseriesColl"}
        ],
        teardown: (st) => {
            // The temporary reshard collection must be dropped before checking metadata integrity.
            const testDB = st.s.getDB("testDbWithSystemResharding");
            testDB["system.resharding.testColl"].drop();
            testDB["system.buckets.resharding.testTimeseriesColl"].drop();
        }
    },
    {
        name: "configNs",
        tasks: (st) => st.configRS.getPrimary().getDB('admin').aggregate([
            {$listCatalog: {}},
            {$match: {db: "config"}},
            {$addFields: {nsToMove: {$concat: ["config.", "$name"]}}},
            {$project: {nsToMove: 1}}
        ])
    }
];

function runTest(configShard) {
    const st = new ShardingTest({shards: 2, configShard});
    const configDB = st.s.getDB("config");

    testCases.forEach(testCase => {
        if (testCase.shouldSkip && testCase.shouldSkip(st.s)) {
            return;
        }
        jsTest.log("Running test " + tojsononeline({testCase: testCase.name, configShard}));

        if (testCase.setup) {
            testCase.setup(st);
        }

        const collectionsInitial = configDB["collections"].find().toArray();
        const tasks = typeof testCase.tasks == "function" ? testCase.tasks(st) : testCase.tasks;

        tasks.forEach(task => {
            if (task.shouldSkip && task.shouldSkip(st.s)) {
                return;
            }
            const expectedError = task.expectedError || ErrorCodes.IllegalOperation;
            jsTest.log("Running task: " + tojsononeline(task) +
                       ", expectedError: " + expectedError);
            const moveRes =
                st.s.adminCommand({moveCollection: task.nsToMove, toShard: st.shard1.shardName});
            assert.commandFailedWithCode(moveRes, expectedError);
            const collectionsAfterMove = configDB["collections"].find().toArray();
            assert.eq(bsonUnorderedFieldsCompare(collectionsInitial, collectionsAfterMove),
                      0,
                      {collectionsInitial, collectionsAfterMove});
        });

        if (testCase.teardown) {
            testCase.teardown(st);
        }
    });
    st.stop();
}

runTest(false /* configShard */);
runTest(true /* configShard */);
