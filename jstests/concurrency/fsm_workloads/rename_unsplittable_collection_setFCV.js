/**
 * Test that setFCV untracks all unsplittable collections during downgrades, while rename
 * operations are running concurrently.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_80,
 *   # Rename operations in sharded clusters can't be executed in transactions.
 *   does_not_support_transactions,
 *   # Unsplittable collections must reside on the primary shard to be untracked.
 *   assumes_balancer_off,
 *   # Downgrading the FCV with a config shard is disallowed.
 *   config_shard_incompatible,
 *   # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible,
 *  ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

export const $config = (function() {
    const data = {
        dbPrefix: 'db',
        dbCount: 2,
        collPrefix: 'collection',
        collCount: 24,
        getRandomDbName: function() {
            return this.dbPrefix + Random.randInt(this.dbCount);
        },
        getRandomCollName: function() {
            return this.collPrefix + Random.randInt(this.collCount);
        }
    };

    const states = {
        init: function(db, collName) {},
        renameUnsplittableColl: function(db, collName) {
            const dbName = this.getRandomDbName();
            const srcCollName = this.getRandomCollName();
            const srcColl = db.getSiblingDB(dbName).getCollection(srcCollName);

            // Do a best-effort pre-check that 'srcColl' actually exists and is unsplittable.
            // This increases the likelihood that we are performing a successful
            // rename operation later on, which will make the test reproduce faster.
            if (!FixtureHelpers.isUnsplittable(srcColl)) {
                return;
            }

            const targetCollName = this.getRandomCollName();
            const targetNs = dbName + "." + targetCollName;

            jsTestLog(`Executing state renameUnsplittableColl: {srcNs: ${
                srcColl.getFullName()}, targetNs: ${targetNs}}`);

            try {
                assert.commandWorked(srcColl.renameCollection(targetCollName));
            } catch (e) {
                if (e.code == ErrorCodes.IllegalOperation) {
                    // The rename operation is allowed to fail with IllegalOperation only if the
                    // original collection name and the target collection name are the same.
                    assert.eq(srcCollName, targetCollName);
                    return;
                }
                if (e.code == ErrorCodes.NamespaceExists ||
                    e.code == ErrorCodes.NamespaceNotFound ||
                    e.code == ErrorCodes.ConflictingOperationInProgress) {
                    return;
                }
                throw e;
            }
        },
        setFCV: function(db, collName) {
            if (this.tid !== 0) {
                // Run setFCV from a single thread only to ensure that the FCV remains stable
                // when checking for the presence of unsplittable collections afterwards.
                return;
            }

            const fcvValues = [lastLTSFCV, lastContinuousFCV, latestFCV];
            const targetFCV = fcvValues[Random.randInt(3)];
            jsTestLog(`Executing state setFCV: {targetFCV: ${targetFCV}}`);

            try {
                assert.commandWorked(
                    db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
            } catch (e) {
                if (e.code === 5147403) {
                    // Invalid fcv transition (e.g lastContinuous -> lastLTS)
                    jsTestLog("setFCV: Invalid transition");
                    return;
                }
                if (e.code === 7428200) {
                    // Cannot upgrade FCV if a previous FCV downgrade stopped in the middle of
                    // cleaning up internal server metadata.
                    assert.eq(latestFCV, targetFCV);
                    jsTestLog(
                        "setFCV: Cannot upgrade FCV if a previous FCV downgrade stopped in the middle \
                        of cleaning up internal server metadata");
                    return;
                }
                if (e.code === 12587) {
                    // Cannot downgrade FCV that requires a collMod command when index builds are
                    // concurrently taking place.
                    jsTestLog(
                        "setFCV: Cannot downgrade the FCV that requires a collMod command when index \
                        builds are concurrently running");
                    return;
                }
                throw e;
            }

            const fcvValue = db.getSiblingDB("admin")
                                 .system.version.findOne({_id: "featureCompatibilityVersion"})
                                 .version;
            const isLatest = fcvValue === latestFCV;
            const hasUnsplittable =
                db.getSiblingDB("config").collections.findOne({unsplittable: true}) !== null;
            assert(!(hasUnsplittable && !isLatest));
        },
        dropUntrackedColl: function(db, collName) {
            const dbName = this.getRandomDbName();
            const collToDrop = db.getSiblingDB(dbName).getCollection(this.getRandomCollName());
            // Return early if 'collToDrop' is a tracked collection. Note that even if a collection
            // is found to be untracked at this point, there is no guarantee that it won't be
            // concurrently dropped and then recreated as an unsplittable collection just after the
            // check. Nevertheless, by using this best-effort approach, untracked collections will
            // be dropped in the majority of cases, making the test converge into the desired state
            // faster.
            if (FixtureHelpers.isTracked(collToDrop)) {
                return;
            }
            jsTestLog(`Executing state dropUntrackedColl: {ns: ${collToDrop.getFullName()}}`);
            assert(collToDrop.drop());
        },
        createUnsplittableColl: function(db, collName) {
            const dbName = this.getRandomDbName();
            const unsplittableCollName = this.getRandomCollName();
            jsTestLog(
                `Executing state createUnsplittableColl: {ns: ${dbName}.${unsplittableCollName}}`);
            try {
                assert.commandWorked(db.getSiblingDB(dbName).runCommand(
                    {createUnsplittableCollection: unsplittableCollName}));
            } catch (e) {
                if (e.code == ErrorCodes.Interrupted || e.code == ErrorCodes.NamespaceExists ||
                    // Creating an unsplittable collection is not supported for FCV < 8.0
                    e.code == ErrorCodes.InvalidOptions) {
                    return;
                }
                throw e;
            }
        }
    };

    const setup = function setup(db, collName, cluster) {
        const shardNames = Object.keys(cluster.getSerializedCluster().shards);
        const numShards = shardNames.length;
        for (var i = 0; i < this.dbCount; i++) {
            const dbName = this.dbPrefix + i;
            const newDb = db.getSiblingDB(dbName);
            newDb.adminCommand({enablesharding: dbName, primaryShard: shardNames[i % numShards]});
        }
    };

    const teardown = function(db, collName, cluster) {
        assert.commandWorked(
            db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    };

    const transitions = {
        init: {
            setFCV: 0.4,
            renameUnsplittableColl: 0.2,
            dropUntrackedColl: 0.2,
            createUnsplittableColl: 0.2
        },
        setFCV: {
            setFCV: 0.4,
            renameUnsplittableColl: 0.2,
            dropUntrackedColl: 0.2,
            createUnsplittableColl: 0.2
        },
        renameUnsplittableColl: {
            setFCV: 0.4,
            renameUnsplittableColl: 0.2,
            dropUntrackedColl: 0.2,
            createUnsplittableColl: 0.2
        },
        dropUntrackedColl: {
            setFCV: 0.4,
            renameUnsplittableColl: 0.2,
            dropUntrackedColl: 0.2,
            createUnsplittableColl: 0.2
        },
        createUnsplittableColl: {
            setFCV: 0.4,
            renameUnsplittableColl: 0.2,
            dropUntrackedColl: 0.2,
            createUnsplittableColl: 0.2
        }
    };

    return {
        threadCount: 12,
        iterations: 64,
        states: states,
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
    };
})();
