// Tests that query settings can only be set through special setQuerySettings command and not
// directly through setClusterParameter.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
//   requires_sharding,
//   requires_replication,
// ]

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let test = (db) => {
    // Creating the collection, because some sharding passthrough suites are failing when
    // explain command is issued on the nonexistent database and collection.
    const coll = assertDropAndRecreateCollection(db, jsTestName());
    const qsutils = new QuerySettingsUtils(db, coll.getName());

    let query = qsutils.makeAggregateQueryInstance({
        pipeline: [
            {$match: {matchKey: 15}},
            {
                $group: {
                    _id: "groupID",
                    values: {$addToSet: "$value"},
                },
            },
        ],
    });
    const querySettings = {
        indexHints: {
            ns: {db: db.getName(), coll: coll.getName()},
            allowedIndexes: ["groupID_1", {$natural: 1}],
        },
    };

    // Reset query settings.
    qsutils.removeAllQuerySettings();

    // Ensure 'setClusterParameter' doesn't accept query settings parameter directly.
    assert.commandFailedWithCode(
        db.adminCommand({
            setClusterParameter: {
                querySettings: [querySettings],
            },
        }),
        ErrorCodes.NoSuchKey,
    );

    // Ensure that 'querySettings' cluster parameter hasn't changed after invoking
    // 'setClusterParameter' command.
    qsutils.assertQueryShapeConfiguration([]);

    // Ensure that query settings can be configured through setQuerySettings command.
    assert.commandWorked(db.adminCommand({setQuerySettings: query, settings: querySettings}));

    // Ensure that 'querySettings' cluster parameter contains QueryShapeConfiguration after
    // invoking setQuerySettings command.
    qsutils.assertQueryShapeConfiguration([qsutils.makeQueryShapeConfiguration(querySettings, query)]);

    // Ensure 'getClusterParameter' doesn't accept query settings parameter directly.
    assert.commandFailedWithCode(db.adminCommand({getClusterParameter: "querySettings"}), ErrorCodes.NoSuchKey);
    assert.commandFailedWithCode(
        db.adminCommand({
            getClusterParameter: ["testIntClusterParameter", "querySettings", "testStrClusterParameter"],
        }),
        ErrorCodes.NoSuchKey,
    );

    // Ensure 'getClusterParameter' doesn't print query settings value with other cluster
    // parameters.
    const clusterParameters = assert.commandWorked(db.adminCommand({getClusterParameter: "*"})).clusterParameters;
    assert(
        !clusterParameters.some((parameter) => parameter._id === "querySettings"),
        "unexpected _id = 'querySettings' in " + tojson(clusterParameters),
    );

    // Cleanup query settings.
    qsutils.removeAllQuerySettings();
};

{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    test(rst.getPrimary().getDB("ReplSetTestDB"));
    rst.stopSet();
}

{
    const st = new ShardingTest({shards: 3, mongos: 1});
    test(st.getDB("ShardingTestDB"));
    st.stop();
}
