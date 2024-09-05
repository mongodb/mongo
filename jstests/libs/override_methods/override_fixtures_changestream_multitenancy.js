// Overrides the JS test exiting 'RepliSetTest' fixture to enable reading the change stream in the
// multitenancy and serverless environment.

import {
    kOverrideConstructor as kOverrideConstructorForRST,
    ReplSetTest
} from "jstests/libs/replsettest.js";
import {
    kOverrideConstructor as kOverrideConstructorForST,
    ShardingTest
} from "jstests/libs/shardingtest.js";

ReplSetTest[kOverrideConstructorForRST] =
    class MultitenantChangeStreamReplSetTest extends ReplSetTest {
    constructor(opts) {
        // Setup the 'serverless' environment if the 'opts' is not a connection string, ie. the
        // replica-set does not already exist and the replica-set is not part of the sharded
        // cluster, ie. 'setParametersMongos' property does not exist.
        const newOpts = typeof opts !== "string" && !TestData.hasOwnProperty("setParametersMongos")
            ? Object.assign({name: "OverridenServerlessChangeStreamReplSet", serverless: true},
                            opts)
            : opts;

        // Call the constructor with the original 'ReplSetTest' to populate 'this' with required
        // fields.
        super(newOpts);
    }

    startSetAsync(options, restart, isMixedVersionCluster) {
        const newOptions = Object.assign({}, options || {});

        let setParameter = {};

        // A change collection does not exist in the config server, do not set any change collection
        // related parameter.
        if (!newOptions.hasOwnProperty("configsvr")) {
            setParameter = {
                // TODO SERVER-68341 Pass multitenancy flags here.
                featureFlagServerlessChangeStreams: true,
                internalChangeStreamUseTenantIdForTesting: true
            };
        }

        newOptions.setParameter = Object.assign({}, newOptions.setParameter, setParameter);
        return super.startSetAsync(newOptions, restart);
    }

    initiate(cfg, initCmd) {
        super.initiate(cfg, initCmd, {doNotWaitForPrimaryOnlyServices: false});

        // Enable the change stream and verify that it is enabled.
        const adminDb = this.getPrimary().getDB("admin");
        assert.commandWorked(adminDb.runCommand({setChangeStreamState: 1, enabled: true}));
        assert.eq(assert.commandWorked(adminDb.runCommand({getChangeStreamState: 1})).enabled,
                  true);

        // Verify that the change stream cursor is getting opened in the change collection.
        const explain = assert.commandWorked(adminDb.getSiblingDB("test").runCommand({
            aggregate: 1,
            pipeline: [{$changeStream: {}}],
            explain: true,
        }));
        assert.eq(explain.stages[0].$cursor.queryPlanner.namespace,
                  'config.system.change_collection');
    }
};

ShardingTest[kOverrideConstructorForST] =
    class MultitenantChangeStreamShardingTest extends ShardingTest {
    constructor(params) {
        super(params);

        // For each shard, enable the change stream.
        this._rs.forEach((shardSvr) => {
            const adminDb = shardSvr.test.getPrimary().getDB("admin");
            assert.commandWorked(adminDb.runCommand({setChangeStreamState: 1, enabled: true}));
            assert.eq(assert.commandWorked(adminDb.runCommand({getChangeStreamState: 1})).enabled,
                      true);

            // Verify that the change stream cursor is getting opened in the change collection.
            const explain = assert.commandWorked(adminDb.getSiblingDB("test").runCommand({
                aggregate: 1,
                pipeline: [{$changeStream: {}}],
                explain: true,
            }));
            assert.eq(explain.stages[0].$cursor.queryPlanner.namespace,
                      'config.system.change_collection');
        });
    }
};
