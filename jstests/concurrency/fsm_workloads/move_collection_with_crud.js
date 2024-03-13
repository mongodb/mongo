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
 *  requires_fcv_72
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

    function calcuateToShard(conn, ns) {
        var config = conn.rsConns.config.getDB('config');
        var unshardedColl = config.collections.findOne({_id: ns});
        var chunk = config.chunks.findOne({uuid: unshardedColl.uuid});
        var currentShard = chunk.shard;

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
        if (TestData.runningWithShardStepdowns) {
            assert.commandWorkedOrFailedWithCode(db.adminCommand(moveCollectionCmdObj),
                                                 [ErrorCodes.SnapshotUnavailable]);
        } else {
            assert.commandWorked(db.adminCommand(moveCollectionCmdObj));
        }

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
                const toShard = calcuateToShard(connCache, coll.getFullName());
                executeMoveCollectionCommand(db, coll, toShard);

                this.moveCollectionCount += 1;
            }
        }
    };

    const transitions = {
        moveCollection: {insert: 1.0},
        insert: {insert: 0.85, moveCollection: 0.15},
    };

    function setup(db, collName, _cluster) {
        print(`Started unshardCollection on ${db + '.' + collName}`);
        assert.commandWorked(db.adminCommand({unshardCollection: db + '.' + collName}));
        print(`Finished unshardCollection on ${db + '.' + collName}`);

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
