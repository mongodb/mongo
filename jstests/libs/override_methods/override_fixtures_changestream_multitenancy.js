// Overrides the JS test exiting 'RepliSetTest' fixture to enable reading the change stream in the
// multitenancy and serverless environment.
(function() {
"use strict";

// Make a copy of the original 'ReplSetTest' fixture.
const originalReplSet = ReplSetTest;

ReplSetTest = function(opts) {
    // Setup the 'serverless' environment if the 'opts' is not a connection string, ie. the
    // replica-set does not already exist and the replica-set is not part of the sharded cluster,
    // ie. 'setParametersMongos' property does not exist.
    const newOpts = typeof opts !== "string" && !TestData.hasOwnProperty("setParametersMongos")
        ? Object.assign({name: "OverridenServerlessChangeStreamReplSet", serverless: true}, opts)
        : opts;

    // Call the constructor with the original 'ReplSetTest' to populate 'this' with required fields.
    originalReplSet.apply(this, [newOpts]);

    // Make a copy of the original 'startSetAsync' function and then override it to include the
    // required parameters.
    this._originalStartSetAsync = this.startSetAsync;
    this.startSetAsync = function(options, restart) {
        const newOptions = Object.assign({}, options || {});

        const fpAssertChangeStreamNssColl =
            tojson({mode: "alwaysOn", data: {collectionName: "system.change_collection"}});

        let setParameter = {};

        // A change collection does not exist in the config server, do not set any change collection
        // related parameter.
        if (!newOptions.hasOwnProperty("configsvr")) {
            setParameter = {
                featureFlagServerlessChangeStreams: true,
                internalChangeStreamUseTenantIdForTesting: true,
                "failpoint.assertChangeStreamNssCollection": fpAssertChangeStreamNssColl,
            };
        }

        newOptions.setParameter = Object.assign({}, newOptions.setParameter, setParameter);
        return this._originalStartSetAsync(newOptions, restart);
    };

    // Make a copy of the original 'initiate' function and then override it to issue
    // 'setChangeStreamState' command.
    this._originalInitiate = this.initiate;
    this.initiate = function(cfg, initCmd) {
        this._originalInitiate(cfg, initCmd, {doNotWaitForPrimaryOnlyServices: false});

        // Enable the change stream and verify that it is enabled.
        const adminDb = this.getPrimary().getDB("admin");
        assert.commandWorked(adminDb.runCommand({setChangeStreamState: 1, enabled: true}));
        assert.eq(assert.commandWorked(adminDb.runCommand({getChangeStreamState: 1})).enabled,
                  true);
    };
};

// Extend the new 'ReplSetTest' fixture with the properties of the original one.
Object.extend(ReplSetTest, originalReplSet);

// Make a copy of the original 'ShardingTest' fixture.
const originalShardingTest = ShardingTest;

ShardingTest = function(params) {
    // Call the original 'ShardingTest' fixture.
    const retShardingTest = originalShardingTest.apply(this, [params]);

    // For each shard, enable the change stream.
    this._rs.forEach((shardSvr) => {
        const adminDb = shardSvr.test.getPrimary().getDB("admin");
        assert.commandWorked(adminDb.runCommand({setChangeStreamState: 1, enabled: true}));
        assert.eq(assert.commandWorked(adminDb.runCommand({getChangeStreamState: 1})).enabled,
                  true);
    });

    return retShardingTest;
};

// Extend the new 'ShardingTest' fixture with the properties of the original one.
Object.extend(ShardingTest, originalShardingTest);
})();