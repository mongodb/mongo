/**
 * Runs moveCollection and CRUD operations concurrently.
 *
 * @tags: [
 *  # The balancer automatically running moveCollection conflicts with running moveCollection here.
 *  assumes_balancer_off,
 *  requires_sharding,
 *  featureFlagUnshardCollection,
 *  featureFlagMoveCollection,
 *  featureFlagReshardingImprovements,
 *  # TODO (SERVER-87812) Remove multiversion_incompatible tag
 *  multiversion_incompatible,
 *  requires_fcv_80
 * ]
 */

export const $config = (function() {
    const kTotalWorkingDocuments = 500;
    const iterations = 20;
    // In the duration moveCollection takes to complete, all the other threads finish running their
    // iterations and terminate. This leaves thread0 as the only thread performing the workload
    // after the first moveCollection. Therefore, we want to limit moveCollection executions to 4.
    const kMaxMoveCollectionExecutions = 4;

    const data = {
        moveCollectionCount: 0,
        primaryShard: undefined,
    };

    /**
     * @summary Takes in a number of documents to create, creates each document.
     * @param {number} numDocs
     * @returns {Array{Object}} an array of documents to be inserted into the collection.
     */
    function createDocuments(numDocs) {
        const documents = Array.from({length: numDocs}).map((_, i) => ({a: i, b: 0}));
        return documents;
    }

    function calculateToShard(conn, ns) {
        var config = conn.rsConns.config.getDB('config');
        var unshardedColl = config.collections.findOne({_id: ns});
        // In case the collection is untracked the current shard is the primary shard.
        let currentShardFn = () => {
            if (unshardedColl === null) {
                return data.primaryShard;
            } else {
                var chunk = config.chunks.findOne({uuid: unshardedColl.uuid});
                if (chunk === null) {
                    return data.primaryShard;
                }
                return chunk.shard;
            }
        };
        var currentShard = currentShardFn();
        var shards = Object.keys(conn.shards);
        var destinationShards = shards.filter(function(shard) {
            if (shard !== currentShard) {
                return shard;
            }
        });

        var toShard = destinationShards[Random.randInt(destinationShards.length)];
        return toShard;
    }

    function executeMoveCollectionCommand(db, coll, toShard) {
        print(`Started moveCollection on ${coll.getFullName()} to shard ${tojson(toShard)}`);

        let moveCollectionCmdObj = {moveCollection: coll.getFullName(), toShard: toShard};
        let acceptedErrors = [
            // Handles the edge case where a collection becomes untracked in the brief moment
            // between being tracked and moved
            ErrorCodes.NamespaceNotFound,
        ];
        if (TestData.runningWithShardStepdowns) {
            acceptedErrors.push(ErrorCodes.SnapshotUnavailable);
        }
        assert.commandWorkedOrFailedWithCode(db.adminCommand(moveCollectionCmdObj), acceptedErrors);
        print(`Finished moveCollection on ${coll.getFullName()} to shard ${tojson(toShard)}`);
    }

    const states = {
        insert: function insert(db, collName, connCache) {
            const coll = db.getCollection(collName);
            print(`Inserting documents to collection ${coll.getFullName()}.`);
            assert.soon(() => {
                try {
                    coll.insert(createDocuments(10));
                    return true;
                } catch (err) {
                    if (err instanceof BulkWriteError && err.hasWriteErrors()) {
                        for (let writeErr of err.getWriteErrors()) {
                            if (writeErr.code == 11000) {
                                // 11000 is a duplicate key error. If the insert generates the same
                                // _id object as another concurrent insert, retry the command.
                                return false;
                            }
                        }
                    }
                    throw err;
                }
            });
            print(`Finished inserting documents.`);
        },
        moveCollection: function moveCollection(db, collName, connCache) {
            const shouldContinueMoveCollection =
                this.moveCollectionCount <= kMaxMoveCollectionExecutions;
            if (this.tid === 0 && shouldContinueMoveCollection) {
                const coll = db.getCollection(collName);
                const toShard = calculateToShard(connCache, coll.getFullName());
                executeMoveCollectionCommand(db, coll, toShard);

                this.moveCollectionCount += 1;
            }
        },
        untrackUnshardedCollection: function untrackUnshardedCollection(db, collName, connCache) {
            const namespace = `${db}.${collName}`;
            print(`Started to untrack collection ${namespace}`);
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({untrackUnshardedCollection: namespace}), [
                    // Handles the case where the collection is not located on its primary
                    ErrorCodes.OperationFailed,
                    // Handles the case where the collection is sharded
                    ErrorCodes.InvalidNamespace,
                    // The command does not exist in pre-8.0 versions
                    ErrorCodes.CommandNotFound,
                ]);
            print(`Untrack collection completed`);
        }
    };

    const transitions = {
        moveCollection: {insert: 1.0},
        untrackUnshardedCollection: {insert: 1.0},
        insert: {insert: 0.75, moveCollection: 0.15, untrackUnshardedCollection: 0.10},
    };

    function setup(db, collName, _cluster) {
        const ns = db + '.' + collName;
        print(`Started unshardCollection on ${ns}`);
        assert.commandWorked(db.adminCommand({unshardCollection: ns}));
        print(`Finished unshardCollection on ${ns}`);

        // Calculate the primary shard
        var unshardedColl = db.getSiblingDB("config").collections.findOne({_id: ns});
        var chunk = db.getSiblingDB("config").chunks.findOne({uuid: unshardedColl.uuid});
        this.primaryShard = chunk.shard;

        const coll = db.getCollection(collName);
        assert.commandWorked(coll.insert(createDocuments(kTotalWorkingDocuments)));
    }

    return {
        threadCount: 15,
        iterations: iterations,
        startState: 'moveCollection',
        states: states,
        transitions: transitions,
        setup: setup,
        data: data,
        passConnectionCache: true
    };
})();
