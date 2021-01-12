"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/sharding/libs/create_sharded_collection_util.js");

/**
 * Test fixture for resharding a sharded collection once.
 *
 * Example usage:
 *
 *      const reshardingTest = new ReshardingTest();
 *      reshardingTest.setup();
 *      const sourceCollection = reshardingTest.createShardedCollection(...);
 *      // ... Do some operations before resharding starts ...
 *      assert.commandWorked(sourceCollection.insert({_id: 0}));
 *      reshardingTest.withReshardingInBackground({...}, () => {
 *          // ... Do some operations during the resharding operation ...
 *          assert.commandWorked(sourceCollection.update({_id: 0}, {$inc: {a: 1}}));
 *      });
 *      reshardingTest.teardown();
 */
var ReshardingTest = class {
    constructor({
        numDonors: numDonors = 1,
        numRecipients: numRecipients = 1,
        reshardInPlace: reshardInPlace = false,
    } = {}) {
        // The @private JSDoc comments cause VS Code to not display the corresponding properties and
        // methods in its autocomplete list. This makes it simpler for test authors to know what the
        // public interface of the ReshardingTest class is.

        /** @private */
        this._numDonors = numDonors;
        /** @private */
        this._numRecipients = numRecipients;
        /** @private */
        this._reshardInPlace = reshardInPlace;
        /** @private */
        this._numShards = this._reshardInPlace ? Math.max(this._numDonors, this._numRecipients)
                                               : this._numDonors + this._numRecipients;

        // Properties set by setup().
        /** @private */
        this._st = undefined;

        // Properties set by createShardedCollection().
        /** @private */
        this._ns = undefined;
        /** @private */
        this._currentShardKey = undefined;
        /** @private */
        this._sourceCollectionUUID = undefined;
        /** @private */
        this._tempNs = undefined;

        // Properties set by startReshardingInBackground() and withReshardingInBackground().
        /** @private */
        this._newShardKey = undefined;
        /** @private */
        this._pauseCoordinatorInSteadyStateFailpoint = undefined;
        /** @private */
        this._reshardingThread = undefined;
        /** @private */
        this._isReshardingActive = false;
    }

    setup() {
        this._st = new ShardingTest({
            mongos: 1,
            config: 1,
            shards: this._numShards,
            rs: {nodes: 2},
            rsOptions: {
                setParameter: {
                    "failpoint.WTPreserveSnapshotHistoryIndefinitely": tojson({mode: "alwaysOn"}),
                }
            },
            manualAddShard: true,
        });

        for (let i = 0; i < this._numShards; ++i) {
            const isDonor = i < this._numDonors;
            const isRecipient = i >= (this._numShards - this._numRecipients);
            assert(isDonor || isRecipient, {
                i,
                numDonors: this._numDonors,
                numRecipients: this._numRecipients,
                numShards: this._numShards,
            });

            // We add a "-donor" or "-recipient" suffix to the shard's name when it has a singular
            // role during the resharding process.
            let shardName = `shard${i}`;
            if (isDonor && !isRecipient) {
                shardName += `-donor${i}`;
            } else if (isRecipient && !isDonor) {
                shardName += `-recipient${i - this._numDonors}`;
            }

            const shard = this._st[`shard${i}`];
            const res = assert.commandWorked(
                this._st.s.adminCommand({addShard: shard.host, name: shardName}));
            shard.shardName = res.shardAdded;
        }
    }

    /** @private */
    _donorShards() {
        return Array.from({length: this._numDonors}, (_, i) => this._st[`shard${i}`]);
    }

    get donorShardNames() {
        return this._donorShards().map(shard => shard.shardName);
    }

    /** @private */
    _recipientShards() {
        return Array
            .from({length: this._numRecipients},
                  (_, i) => this._st[`shard${this._numShards - 1 - i}`])
            .reverse();
    }

    get recipientShardNames() {
        return this._recipientShards().map(shard => shard.shardName);
    }

    /**
     * Shards a non-existing collection using the specified shard key and chunk ranges.
     *
     * @param chunks - an array of
     * {min: <shardKeyValue0>, max: <shardKeyValue1>, shard: <shardName>} objects. The chunks must
     * form a partition of the {shardKey: MinKey} --> {shardKey: MaxKey} space.
     */
    createShardedCollection({ns, shardKeyPattern, chunks}) {
        this._ns = ns;
        this._currentShardKey = Object.assign({}, shardKeyPattern);

        const sourceCollection = this._st.s.getCollection(ns);
        const sourceDB = sourceCollection.getDB();

        CreateShardedCollectionUtil.shardCollectionWithChunks(
            sourceCollection, shardKeyPattern, chunks);

        this._sourceCollectionUUID =
            getUUIDFromListCollections(sourceDB, sourceCollection.getName());
        const sourceCollectionUUIDString = extractUUIDFromObject(this._sourceCollectionUUID);

        this._tempNs = `${sourceDB.getName()}.system.resharding.${sourceCollectionUUIDString}`;

        // mongos won't know about the temporary resharding collection and will therefore assume the
        // collection is unsharded. We configure one of the recipient shards to be the primary shard
        // for the database so mongos still ends up routing operations to a shard which owns the
        // temporary resharding collection.
        this._st.ensurePrimaryShard(sourceDB.getName(), this.recipientShardNames[0]);

        return sourceCollection;
    }

    /**
     * Reshards an existing collection using the specified new shard key and new chunk ranges.
     *
     * @param newChunks - an array of
     * {min: <shardKeyValue0>, max: <shardKeyValue1>, shard: <shardName>} objects. The chunks must
     * form a partition of the {shardKey: MinKey} --> {shardKey: MaxKey} space.
     *
     * @deprecated prefer using the withReshardingInBackground() method instead.
     */
    startReshardingInBackground({newShardKeyPattern, newChunks}) {
        this._startReshardingInBackgroundAndAllowCommandFailure({newShardKeyPattern, newChunks},
                                                                ErrorCodes.OK);
    }

    /** @private */
    _startReshardingInBackgroundAndAllowCommandFailure({newShardKeyPattern, newChunks},
                                                       expectedErrorCode) {
        newChunks = newChunks.map(
            chunk => ({min: chunk.min, max: chunk.max, recipientShardId: chunk.shard}));

        this._newShardKey = Object.assign({}, newShardKeyPattern);

        this._pauseCoordinatorInSteadyStateFailpoint = configureFailPoint(
            this._st.configRS.getPrimary(), "reshardingPauseCoordinatorInSteadyState");

        const commandDoneSignal = new CountDownLatch(1);

        this._reshardingThread = new Thread(
            function(
                host, ns, newShardKeyPattern, newChunks, commandDoneSignal, expectedErrorCode) {
                const conn = new Mongo(host);
                const res = conn.adminCommand({
                    reshardCollection: ns,
                    key: newShardKeyPattern,
                    _presetReshardedChunks: newChunks,
                });
                commandDoneSignal.countDown();

                if (expectedErrorCode === ErrorCodes.OK) {
                    assert.commandWorked(res);
                } else {
                    assert.commandFailedWithCode(res, expectedErrorCode);
                }
            },
            this._st.s.host,
            this._ns,
            newShardKeyPattern,
            newChunks,
            commandDoneSignal,
            expectedErrorCode);

        this._reshardingThread.start();
        this._isReshardingActive = true;

        return commandDoneSignal;
    }

    /**
     * Reshards an existing collection using the specified new shard key and new chunk ranges.
     *
     * @param newChunks - an array of
     * {min: <shardKeyValue0>, max: <shardKeyValue1>, shard: <shardName>} objects. The chunks must
     * form a partition of the {shardKey: MinKey} --> {shardKey: MaxKey} space.
     *
     * @param duringReshardingFn - a function which optionally accepts the temporary resharding
     * namespace string. It is only guaranteed to be called after mongos has started running the
     * reshardCollection command. Callers should use DiscoverTopology.findConnectedNodes() to
     * introspect the state of the donor or recipient shards if they need more specific
     * synchronization.
     *
     * @param expectedErrorCode - the expected response code for the reshardCollection command.
     * Callers of interruptReshardingThread() will want to set this to ErrorCodes.Interrupted, for
     * example.
     */
    withReshardingInBackground({newShardKeyPattern, newChunks},
                               duringReshardingFn = (tempNs) => {},
                               expectedErrorCode = ErrorCodes.OK) {
        const commandDoneSignal = this._startReshardingInBackgroundAndAllowCommandFailure(
            {newShardKeyPattern, newChunks}, expectedErrorCode);

        assert.soon(() => {
            const op = this._findReshardingCommandOp();
            return op !== undefined ||
                (expectedErrorCode !== ErrorCodes.OK && commandDoneSignal.getCount() === 0);
        }, "failed to find reshardCollection in $currentOp output");

        this._callFunctionSafely(() => duringReshardingFn(this._tempNs));
        this._checkConsistencyAndPostState(expectedErrorCode);
    }

    /** @private */
    _findReshardingCommandOp() {
        return this._st.admin
            .aggregate([
                {$currentOp: {allUsers: true, localOps: true}},
                {$match: {"command.reshardCollection": this._ns}},
            ])
            .toArray()[0];
    }

    /**
     * Wrapper around invoking a 0-argument function to make test failures less confusing.
     *
     * This helper attempts to disable the reshardingPauseCoordinatorInSteadyState failpoint when an
     * exception is thrown to prevent the mongo shell from hanging (really the config server) on top
     * of having a JavaScript error.
     *
     * This helper attempts to interrupt and join the resharding thread when an exception is thrown
     * to prevent the mongo shell from aborting on top of having a JavaScript error.
     *
     * @private
     */
    _callFunctionSafely(fn) {
        try {
            fn();
        } catch (duringReshardingError) {
            try {
                this._pauseCoordinatorInSteadyStateFailpoint.off();
            } catch (disableFailpointError) {
                print(`Ignoring error from disabling the resharding coordinator failpoint: ${
                    tojson(disableFailpointError)}`);

                print("The config server primary and the mongo shell along with it are expected" +
                      " to hang due to the resharding coordinator being left uninterrupted");
            }

            try {
                this.interruptReshardingThread();

                try {
                    this._reshardingThread.join();
                } catch (joinError) {
                    print(`Ignoring error from the resharding thread: ${tojson(joinError)}`);
                }
            } catch (killOpError) {
                print(`Ignoring error from sending killOp to the reshardCollection command: ${
                    tojson(killOpError)}`);

                print("The mongo shell is expected to abort due to the resharding thread being" +
                      " left unjoined");
            }

            throw duringReshardingError;
        }
    }

    interruptReshardingThread() {
        const op = this._findReshardingCommandOp();
        assert.neq(undefined, op, "failed to find reshardCollection in $currentOp output");
        assert.commandWorked(this._st.admin.killOp(op.opid));
    }

    /** @private */
    _checkConsistencyAndPostState(expectedErrorCode) {
        if (expectedErrorCode === ErrorCodes.OK) {
            this._callFunctionSafely(() => {
                // We use the reshardingPauseCoordinatorInSteadyState failpoint so that any
                // intervening writes performed on the sharded collection (from when the resharding
                // operation had started until now) are eventually applied by the recipient shards.
                // We then use the reshardingPauseCoordinatorBeforeCommit to wait for all of the
                // recipient shards to have applied through all of the oplog entries from all of the
                // donor shards.
                this._pauseCoordinatorInSteadyStateFailpoint.wait();
                const pauseCoordinatorBeforeCommitFailpoint =
                    configureFailPoint(this._pauseCoordinatorInSteadyStateFailpoint.conn,
                                       "reshardingPauseCoordinatorBeforeCommit");

                this._pauseCoordinatorInSteadyStateFailpoint.off();
                pauseCoordinatorBeforeCommitFailpoint.wait();

                this._checkConsistency();

                pauseCoordinatorBeforeCommitFailpoint.off();
            });
        } else {
            this._callFunctionSafely(() => {
                this._pauseCoordinatorInSteadyStateFailpoint.off();
            });
        }

        this._reshardingThread.join();
        this._isReshardingActive = false;

        // TODO SERVER-52838: Call _checkPostState() when donor and recipient shards clean up their
        // local metadata on error.
        if (expectedErrorCode === ErrorCodes.OK) {
            this._checkPostState(expectedErrorCode);
        }
    }

    /** @private */
    _checkConsistency() {
        const nsCursor = this._st.s.getCollection(this._ns).find().sort({_id: 1});
        const tempNsCursor = this._st.s.getCollection(this._tempNs).find().sort({_id: 1});

        const diff = ((diff) => {
            return {
                docsWithDifferentContents: diff.docsWithDifferentContents.map(
                    ({first, second}) => ({original: first, resharded: second})),
                docsExtraAfterResharding: diff.docsMissingOnFirst,
                docsMissingAfterResharding: diff.docsMissingOnSecond,
            };
        })(DataConsistencyChecker.getDiff(nsCursor, tempNsCursor));

        assert.eq(diff, {
            docsWithDifferentContents: [],
            docsExtraAfterResharding: [],
            docsMissingAfterResharding: [],
        });
    }

    /** @private */
    _checkPostState(expectedErrorCode) {
        this._checkCoordinatorPostState(expectedErrorCode);

        for (let recipient of this._recipientShards()) {
            this._checkRecipientPostState(recipient, expectedErrorCode);
        }

        for (let donor of this._donorShards()) {
            this._checkDonorPostState(donor, expectedErrorCode);
        }
    }

    /** @private */
    _checkCoordinatorPostState(expectedErrorCode) {
        assert.eq([],
                  this._st.config.reshardingOperations.find({nss: this._ns}).toArray(),
                  "expected config.reshardingOperations to be empty, but found it wasn't");

        assert.eq([],
                  this._st.config.collections.find({reshardingFields: {$exists: true}}).toArray(),
                  "expected there to be no config.collections entries with 'reshardingFields' set");

        assert.eq([],
                  this._st.config.collections.find({allowMigrations: {$exists: true}}).toArray(),
                  "expected there to be no config.collections entries with 'allowMigrations' set");

        assert.eq([],
                  this._st.config.collections.find({_id: this._tempNs}).toArray(),
                  "expected there to not be a config.collections entry for the temporary" +
                      " resharding collection");

        const collEntry = this._st.config.collections.findOne({_id: this._ns});
        assert.neq(null, collEntry, `didn't find config.collections entry for ${this._ns}`);

        if (expectedErrorCode === ErrorCodes.OK) {
            assert.eq(this._newShardKey,
                      collEntry.key,
                      "shard key pattern didn't change despite resharding having succeeded");
            assert.neq(this._sourceCollectionUUID,
                       collEntry.uuid,
                       "collection UUID didn't change despite resharding having succeeded");
        } else {
            assert.eq(this._currentShardKey,
                      collEntry.key,
                      "shard key pattern changed despite resharding having failed");
            assert.eq(this._sourceCollectionUUID,
                      collEntry.uuid,
                      "collection UUID changed despite resharding having failed");
        }
    }

    /** @private */
    _checkRecipientPostState(recipient, expectedErrorCode) {
        assert.eq(
            null,
            recipient.getCollection(this._tempNs).exists(),
            `expected the temporary resharding collection to not exist, but found it does on ${
                recipient.shardName}`);

        const collInfo = recipient.getCollection(this._ns).exists();
        const isAlsoDonor = this._donorShards().includes(recipient);
        if (expectedErrorCode === ErrorCodes.OK) {
            assert.neq(null,
                       collInfo,
                       `collection doesn't exist on ${
                           recipient.shardName} despite resharding having succeeded`);
            assert.neq(this._sourceCollectionUUID,
                       collInfo.info.uuid,
                       `collection UUID didn't change on ${
                           recipient.shardName} despite resharding having succeeded`);
        } else if (expectedErrorCode !== ErrorCodes.OK && !isAlsoDonor) {
            assert.eq(
                null,
                collInfo,
                `collection exists on ${recipient.shardName} despite resharding having failed`);
        }

        const localRecipientOpsNs = "config.localReshardingOperations.recipient";
        let res;
        assert.soon(
            () => {
                res = recipient.getCollection(localRecipientOpsNs).find().toArray();
                return res.length === 0;
            },
            () => `${localRecipientOpsNs} document wasn't cleaned up on ${recipient.shardName}: ${
                tojson(res)}`);
    }

    /** @private */
    _checkDonorPostState(donor, expectedErrorCode) {
        const collInfo = donor.getCollection(this._ns).exists();
        const isAlsoRecipient = this._recipientShards().includes(donor);
        if (expectedErrorCode === ErrorCodes.OK && !isAlsoRecipient) {
            assert.eq(
                null,
                collInfo,
                `collection exists on ${donor.shardName} despite resharding having succeeded`);
        } else if (expectedErrorCode !== ErrorCodes.OK) {
            assert.neq(
                null,
                collInfo,
                `collection doesn't exist on ${donor.shardName} despite resharding having failed`);
            assert.eq(
                this._sourceCollectionUUID,
                collInfo.info.uuid,
                `collection UUID changed on ${donor.shardName} despite resharding having failed`);
        }

        const localDonorOpsNs = "config.localReshardingOperations.donor";
        let res;
        assert.soon(
            () => {
                res = donor.getCollection(localDonorOpsNs).find().toArray();
                return res.length === 0;
            },
            () => `${localDonorOpsNs} document wasn't cleaned up on ${donor.shardName}: ${
                tojson(res)}`);
    }

    teardown() {
        if (this._isReshardingActive) {
            this._checkConsistencyAndPostState(ErrorCodes.OK);
        }

        this._st.stop();
    }
};
