/**
 * Runs reshardCollection and CRUD operations (insert, update, findAndModify, delete, read)
 * concurrently.
 * The collection carries a set of secondary indexes so that the resharding recipient has a
 * non-trivial building-index phase.
 *
 * @tags: [
 *  requires_sharding,
 *  # TODO(SERVER-119777): Ensure test does not leak cursors.
 *  can_leak_idle_cursors,
 * ]
 */

export const $config = (function () {
    const shardKeys = [{a: 1}, {b: 1}];

    const data = {
        shardKey: shardKeys[0],
        currentShardKeyIndex: 0,
        reshardingCount: 0,
    };

    const iterations = 25;
    const kTotalWorkingDocuments = 5000;
    const kMaxReshardingExecutions = TestData.runningWithShardStepdowns ? 4 : iterations;
    const kNumSecondaryIndexes = 20;
    const secondaryIndexFields = Array.from({length: kNumSecondaryIndexes}, (_, i) => "i" + i);

    // Resharding drops the old collection once it commits, which kills the plans of the operations
    // still running against it.
    const kAcceptableCrudErrors = [ErrorCodes.QueryPlanKilled];

    /**
     * @summary Takes in a number of documents to create, creates each document. With two properties
     * being equal to the index, one counter property, and one property per secondary index.
     * @param {number} numDocs
     * @returns {Array{Object}} an array of documents to be inserted into the collection.
     */
    function createDocuments(numDocs) {
        const documents = Array.from({length: numDocs}).map((_, i) => {
            let value = Random.randInt(kTotalWorkingDocuments);
            const doc = {a: value, b: value, c: 0, d: i};
            secondaryIndexFields.forEach((field) => {
                doc[field] = value++;
            });
            return doc;
        });
        return documents;
    }

    /**
     * @summary Returns a filter matching the documents created by createDocuments() for one value.
     * The values of the shard key fields ('a' and 'b') are never modified by this workload, so a
     * filter on both of them is fully targeted whichever of the two shard keys is the current one.
     * That keeps every single-document write retryable under stepdowns.
     * @returns {Object} a query filter.
     */
    function randomDocumentFilter() {
        const value = Random.randInt(kTotalWorkingDocuments);
        return {a: value, b: value};
    }

    function randomSecondaryIndexField() {
        return secondaryIndexFields[Random.randInt(kNumSecondaryIndexes)];
    }

    function executeReshardCommand(db, collName, newShardKey, forceRedistribution) {
        const coll = db.getCollection(collName);
        print(
            `Started Resharding Collection ${coll.getFullName()}. New Shard Key ${tojson(
                newShardKey,
            )}, Same key resharding ${forceRedistribution}`,
        );
        let reshardCollectionCmd = {
            reshardCollection: coll.getFullName(),
            key: newShardKey,
            numInitialChunks: 1,
        };
        if (forceRedistribution) {
            reshardCollectionCmd.forceRedistribution = forceRedistribution;
            // TODO(SERVER-131275): Remove reshardingUUID once the reshardCollectionCoordinator supports retryability.
            reshardCollectionCmd.reshardingUUID = UUID();
        }
        if (TestData.runningWithShardStepdowns) {
            assert.commandWorkedOrFailedWithCode(db.adminCommand(reshardCollectionCmd), [
                ErrorCodes.SnapshotUnavailable,
            ]);
        } else {
            assert.commandWorked(db.adminCommand(reshardCollectionCmd));
        }
        print(
            `Finished Resharding Collection ${coll.getFullName()}. New Shard Key ${tojson(newShardKey)}`,
        );
    }

    const states = {
        insert: function insert(db, collName) {
            const totalDocumentsToInsert = 5;
            const res = db.runCommand({
                insert: collName,
                documents: createDocuments(totalDocumentsToInsert),
            });
            // A DuplicateKey write error is possible when a concurrent insert generates the same
            // _id object.
            assert.commandWorkedOrFailedWithCode(res, [
                ...kAcceptableCrudErrors,
                ErrorCodes.DuplicateKey,
            ]);
        },
        update: function update(db, collName) {
            const field = randomSecondaryIndexField();
            const res = db.runCommand({
                update: collName,
                updates: [
                    {
                        q: randomDocumentFilter(),
                        u: {$inc: {c: 1}, $set: {[field]: Random.randInt(kTotalWorkingDocuments)}},
                        multi: false,
                    },
                ],
            });
            assert.commandWorkedOrFailedWithCode(res, kAcceptableCrudErrors);
        },
        findAndModify: function findAndModify(db, collName) {
            const field = randomSecondaryIndexField();
            const res = db.runCommand({
                findAndModify: collName,
                query: randomDocumentFilter(),
                update: {$inc: {c: 1}, $unset: {[field]: 1}},
                new: true,
            });
            assert.commandWorkedOrFailedWithCode(res, kAcceptableCrudErrors);
        },
        delete: function (db, collName) {
            const res = db.runCommand({
                delete: collName,
                deletes: [{q: randomDocumentFilter(), limit: 1}],
            });
            assert.commandWorkedOrFailedWithCode(res, kAcceptableCrudErrors);
        },
        read: function read(db, collName) {
            const field = randomSecondaryIndexField();
            const value = Random.randInt(kTotalWorkingDocuments);
            const res = db.runCommand({
                find: collName,
                filter: {[field]: value},
                hint: {[field]: 1},
                singleBatch: true,
                limit: 200,
            });
            assert.commandWorkedOrFailedWithCode(res, kAcceptableCrudErrors);
        },
        reshardCollection: function reshardCollection(db, collName) {
            //'reshardingMinimumOperationDurationMillis' is set to 30 seconds when there are
            // stepdowns. So in order to limit the overall time for the test, we limit the number of
            // resharding operations to kMaxReshardingExecutions.
            const shouldContinueResharding = this.reshardingCount <= kMaxReshardingExecutions;
            if (this.tid === 0 && shouldContinueResharding) {
                const currentShardKeyIndex = this.currentShardKeyIndex;
                const newIndex = (currentShardKeyIndex + 1) % shardKeys.length;
                const shardKey = shardKeys[newIndex];

                executeReshardCommand(db, collName, shardKey, false /*forceRedistribution*/);
                // If resharding fails with SnapshopUnavailable, then this will be incorrect. But
                // its fine since reshardCollection will succeed if the new shard key matches the
                // existing one.
                this.currentShardKeyIndex = newIndex;
                this.reshardingCount += 1;
            }
        },
        reshardCollectionSameKey: function reshardCollectionSameKey(db, collName) {
            const shouldContinueResharding = this.reshardingCount <= kMaxReshardingExecutions;
            if (this.tid === 0 && shouldContinueResharding) {
                const currentShardKeyIndex = this.currentShardKeyIndex;
                const newIndex = this._allowSameKeyResharding
                    ? currentShardKeyIndex
                    : (currentShardKeyIndex + 1) % shardKeys.length;
                const shardKey = shardKeys[newIndex];

                executeReshardCommand(db, collName, shardKey, this._allowSameKeyResharding);
                // If resharding fails with SnapshopUnavailable, then this will be incorrect. But
                // its fine since reshardCollection will succeed if the new shard key matches the
                // existing one.
                this.currentShardKeyIndex = newIndex;
                this.reshardingCount += 1;
            }
        },
        checkReshardingMetrics: function checkReshardingMetrics(db, collName) {
            const ns = db.getName() + "." + collName;
            const currentOps = db
                .getSiblingDB("admin")
                .aggregate([
                    {$currentOp: {allUsers: true, localOps: false}},
                    {
                        $match: {
                            type: "op",
                            "originatingCommand.reshardCollection": ns,
                            recipientState: {$exists: true},
                        },
                    },
                ])
                .toArray();
            currentOps.forEach((op) => {
                print(
                    "Checking resharding metrics " +
                        tojsononeline({
                            approxDocumentsToCopy: op.approxDocumentsToCopy,
                            documentsCopied: op.documentsCopied,
                            approxBytesToCopy: op.approxBytesToCopy,
                            bytesCopied: op.bytesCopied,
                            oplogEntriesFetched: op.oplogEntriesFetched,
                            oplogEntriesApplied: op.oplogEntriesApplied,
                        }),
                );
                assert.gte(op.oplogEntriesFetched, op.oplogEntriesApplied, op);
            });
        },
    };

    const nextStateProbabilities = {
        insert: 0.1,
        update: 0.1,
        findAndModify: 0.1,
        delete: 0.1,
        read: 0.1,
        reshardCollection: 0.2,
        reshardCollectionSameKey: 0.2,
        checkReshardingMetrics: 0.1,
    };

    const transitions = Object.fromEntries(
        Object.keys(nextStateProbabilities).map((state) => [
            state,
            Object.assign({}, nextStateProbabilities),
        ]),
    );

    function setup(db, collName, _cluster) {
        const coll = db.getCollection(collName);
        assert.commandWorked(coll.insert(createDocuments(kTotalWorkingDocuments)));
        assert.commandWorked(
            db.runCommand({
                createIndexes: collName,
                indexes: secondaryIndexFields.map((field) => ({
                    key: {[field]: 1},
                    name: field + "_idx",
                })),
            }),
        );
        this._allowSameKeyResharding = true;
    }

    return {
        threadCount: 20,
        iterations: iterations,
        startState: "reshardCollection",
        states: states,
        transitions: transitions,
        setup: setup,
        data: data,
    };
})();
