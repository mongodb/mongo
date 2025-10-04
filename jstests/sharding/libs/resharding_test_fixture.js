import {
    areViewlessTimeseriesEnabled,
    getTimeseriesBucketsColl,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getDBNameAndCollNameFromFullNamespace} from "jstests/libs/namespace_utils.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {awaitRSClientHosts} from "jstests/replsets/rslib.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

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
export var ReshardingTest = class {
    constructor({
        numDonors: numDonors = 1,
        numRecipients: numRecipients = 1,
        reshardInPlace: reshardInPlace = false,
        minimumOperationDurationMS: minimumOperationDurationMS = undefined,
        criticalSectionTimeoutMS: criticalSectionTimeoutMS = 24 * 60 * 60 * 1000 /* 1 day */,
        periodicNoopIntervalSecs: periodicNoopIntervalSecs = undefined,
        writePeriodicNoops: writePeriodicNoops = undefined,
        enableElections: enableElections = false,
        chainingAllowed: chainingAllowed = true,
        logComponentVerbosity: logComponentVerbosity = undefined,
        oplogSize: oplogSize = undefined,
        maxNumberOfTransactionOperationsInSingleOplogEntry:
            maxNumberOfTransactionOperationsInSingleOplogEntry = undefined,
        configShard: configShard = false,
        wiredTigerConcurrentWriteTransactions: wiredTigerConcurrentWriteTransactions = undefined,
        reshardingOplogBatchTaskCount: reshardingOplogBatchTaskCount = undefined,
        ttlMonitorSleepSecs: ttlMonitorSleepSecs = undefined,
        initiateWithDefaultElectionTimeout: initiateWithDefaultElectionTimeout = false,
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
        this._numShards = this._reshardInPlace
            ? Math.max(this._numDonors, this._numRecipients)
            : this._numDonors + this._numRecipients;
        /** @private */
        this._minimumOperationDurationMS = minimumOperationDurationMS;
        /** @private */
        this._criticalSectionTimeoutMS = criticalSectionTimeoutMS;
        /** @private */
        this._periodicNoopIntervalSecs = periodicNoopIntervalSecs;
        /** @private */
        this._writePeriodicNoops = writePeriodicNoops;
        /** @private */
        this._enableElections = enableElections;
        /** @private */
        this._chainingAllowed = chainingAllowed;
        /** @private */
        this._logComponentVerbosity = logComponentVerbosity;
        this._oplogSize = oplogSize;
        this._maxNumberOfTransactionOperationsInSingleOplogEntry = maxNumberOfTransactionOperationsInSingleOplogEntry;
        this._configShard = configShard || jsTestOptions().configShard;
        this._wiredTigerConcurrentWriteTransactions = wiredTigerConcurrentWriteTransactions;
        this._reshardingOplogBatchTaskCount = reshardingOplogBatchTaskCount;
        this._ttlMonitorSleepSecs = ttlMonitorSleepSecs;
        this._initiateWithDefaultElectionTimeout = initiateWithDefaultElectionTimeout;
        /** @private */
        this._opType = "reshardCollection";

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
        /** @private */
        this._primaryShardName = undefined;
        /** @private */
        this._underlyingSourceNs = undefined;

        // Properties set by startReshardingInBackground() and withReshardingInBackground().
        /** @private */
        this._newShardKey = undefined;
        /** @private */
        this._pauseCoordinatorBeforeBlockingWritesFailpoints = [];
        /** @private */
        this._pauseCoordinatorBeforeDecisionPersistedFailpoints = [];
        /** @private */
        this._pauseCoordinatorBeforeCompletionFailpoints = [];
        /** @private */
        this._reshardingThread = undefined;
        /** @private */
        this._isReshardingActive = false;
        /** @private */
        this._commandDoneSignal = undefined;
    }

    setup() {
        const mongosOptions = {setParameter: {}};
        let configOptions = {setParameter: {}};
        let rsOptions = {setParameter: {}};
        if (this._oplogSize) {
            rsOptions.oplogSize = this._oplogSize;
        }
        const configReplSetTestOptions = {};

        let nodesPerShard = 2;
        // Use the shard default in config shard mode since the config server will be a shard.
        let nodesPerConfigRs = this._configShard ? 2 : 1;

        if (this._enableElections) {
            nodesPerShard = 3;
            nodesPerConfigRs = 3;

            // Increase the likelihood that writes which aren't yet majority-committed end up
            // getting rolled back.
            rsOptions.settings = {
                catchUpTimeoutMillis: 0,
                chainingAllowed: this._chainingAllowed,
            };
            configReplSetTestOptions.settings = {
                catchUpTimeoutMillis: 0,
                chainingAllowed: this._chainingAllowed,
            };
            this._initiateWithDefaultElectionTimeout = true;

            rsOptions.setParameter.enableElectionHandoff = 0;
            configOptions.setParameter.enableElectionHandoff = 0;

            // The server conservatively sets the minimum visible timestamp of collections created
            // after the oldest_timestamp to be the stable_timestamp. Furthermore, there is no
            // guarantee the oldest_timestamp will advance past the creation timestamp of the source
            // sharded collection. This means that after a donor shard restarts an atClusterTime
            // read at the cloneTimestamp on it would fail with SnapshotUnavailable. We enable the
            // following failpoint so the minimum visible timestamp is set to the oldest_timestamp
            // regardless. Note that this is safe for resharding tests to do because the source
            // sharded collection is guaranteed to exist in the collection catalog at the
            // cloneTimestamp and tests involving elections do not run operations which would bump
            // the minimum visible timestamp (e.g. creating or dropping indexes).
            rsOptions.setParameter["failpoint.setMinVisibleForAllCollectionsToOldestOnStartup"] = tojson({
                mode: {"times": 1},
            });
        }

        if (this._minimumOperationDurationMS !== undefined) {
            configOptions.setParameter.reshardingMinimumOperationDurationMillis = this._minimumOperationDurationMS;
        }

        if (this._criticalSectionTimeoutMS !== -1) {
            configOptions.setParameter.reshardingCriticalSectionTimeoutMillis = this._criticalSectionTimeoutMS;
        }

        if (this._periodicNoopIntervalSecs !== undefined) {
            rsOptions.setParameter.periodicNoopIntervalSecs = this._periodicNoopIntervalSecs;
        }

        if (this._writePeriodicNoops !== undefined) {
            rsOptions.setParameter.writePeriodicNoops = this._writePeriodicNoops;
        }

        if (this._logComponentVerbosity !== undefined) {
            rsOptions.setParameter.logComponentVerbosity = this._logComponentVerbosity;
            configOptions.setParameter.logComponentVerbosity = this._logComponentVerbosity;
            mongosOptions.setParameter.logComponentVerbosity = this._logComponentVerbosity;
        }

        if (this._maxNumberOfTransactionOperationsInSingleOplogEntry !== undefined) {
            rsOptions.setParameter.maxNumberOfTransactionOperationsInSingleOplogEntry =
                this._maxNumberOfTransactionOperationsInSingleOplogEntry;
        }

        if (this._configShard) {
            // ShardingTest does not currently support deep merging of options, so merge the set
            // parameters for config and replica sets here.
            rsOptions.setParameter = Object.merge(rsOptions.setParameter, configOptions.setParameter);
            configOptions.setParameter = Object.merge(configOptions.setParameter, rsOptions.setParameter);
        }

        if (this._wiredTigerConcurrentWriteTransactions !== undefined) {
            rsOptions.setParameter.storageEngineConcurrencyAdjustmentAlgorithm = "fixedConcurrentTransactions";
            rsOptions.setParameter.wiredTigerConcurrentWriteTransactions = this._wiredTigerConcurrentWriteTransactions;
        }

        if (this._reshardingOplogBatchTaskCount !== undefined) {
            rsOptions.setParameter.reshardingOplogBatchTaskCount = this._reshardingOplogBatchTaskCount;
        }

        if (this._ttlMonitorSleepSecs !== undefined) {
            rsOptions.setParameter.ttlMonitorSleepSecs = this._ttlMonitorSleepSecs;
        }

        this._st = new ShardingTest({
            mongos: 1,
            mongosOptions,
            config: nodesPerConfigRs,
            configOptions,
            shards: this._numShards,
            rs: {nodes: nodesPerShard},
            rsOptions,
            configReplSetTestOptions,
            manualAddShard: true,
            configShard: this._configShard,
            initiateWithDefaultElectionTimeout: this._initiateWithDefaultElectionTimeout,
        });

        for (let i = 0; i < this._numShards; ++i) {
            const isDonor = i < this._numDonors;
            const isRecipient = i >= this._numShards - this._numRecipients;
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
            if (this._configShard && i == 0) {
                assert.commandWorked(this._st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));
                shard.shardName = "config";
            } else {
                const res = assert.commandWorked(this._st.s.adminCommand({addShard: shard.host, name: shardName}));
                shard.shardName = res.shardAdded;
            }
        }

        const buildInfo = assert.commandWorked(this._st.s0.adminCommand({"buildInfo": 1}));
        const isSanitizerEnabled = buildInfo.buildEnvironment.ccflags.includes("-fsanitize");

        if (isSanitizerEnabled) {
            assert.commandWorked(this._st.s.adminCommand({setParameter: 1, maxRoundsWithoutProgressParameter: 10}));
        }

        // In order to enable random failovers, initialize Random's seed if it has not already been
        // done.
        if (!Random.isInitialized()) {
            Random.setRandomSeed();
        }
    }

    /** @private */
    _donorShards() {
        return Array.from({length: this._numDonors}, (_, i) => this._st[`shard${i}`]);
    }

    get donorShardNames() {
        return this._donorShards().map((shard) => shard.shardName);
    }

    /** @private */
    _recipientShards() {
        return Array.from(
            {length: this._numRecipients},
            (_, i) => this._st[`shard${this._numShards - 1 - i}`],
        ).reverse();
    }

    get recipientShardNames() {
        return this._recipientShards().map((shard) => shard.shardName);
    }

    get configShardName() {
        return "config";
    }

    /** @private */
    _allReplSetTests() {
        return [
            {shardName: this.configShardName, rs: this._st.configRS},
            ...Array.from({length: this._numShards}, (_, i) => this._st[`shard${i}`]),
        ];
    }

    getReplSetForShard(shardName) {
        const res = this._allReplSetTests().find((shardInfo) => shardInfo.shardName === shardName);
        return res.rs;
    }

    /**
     * Creates an unsharded collection with unsplittable: true
     */
    createUnshardedCollection({ns, primaryShardName = this.donorShardNames[0], collOptions = {}}) {
        this._ns = ns;
        this._currentShardKey = Object.assign({_id: 1});
        const [dbName, collName] = getDBNameAndCollNameFromFullNamespace(ns);

        assert.commandWorked(this._st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}));
        assert.commandWorked(this._st.s.getDB(dbName).runCommand(Object.merge({create: collName}, collOptions)));

        const sourceCollection = this._st.s.getCollection(ns);
        const sourceDB = sourceCollection.getDB();

        this._primaryShardName = primaryShardName;

        let tempCollNamePrefix = "system.resharding";
        // TODO SERVER-101784 simplify once only viewless timeseries collections exist.
        if (collOptions.timeseries !== undefined && !areViewlessTimeseriesEnabled(this._st.s)) {
            let bucketUUID = getUUIDFromListCollections(sourceDB, getTimeseriesBucketsColl(sourceCollection.getName()));

            assert.neq(bucketUUID, null, `can't find ns: ${this._ns} after creating chunks`);

            this._sourceCollectionUUID = bucketUUID;

            tempCollNamePrefix = getTimeseriesBucketsColl("resharding");
            this._underlyingSourceNs = `${sourceDB.getName()}.${getTimeseriesBucketsColl(sourceCollection.getName())}`;
        } else {
            this._sourceCollectionUUID = getUUIDFromListCollections(sourceDB, collName);
            this._underlyingSourceNs = this._ns;
        }

        const sourceCollectionUUIDString = extractUUIDFromObject(this._sourceCollectionUUID);
        this._tempNs = `${sourceDB.getName()}.${tempCollNamePrefix}.${sourceCollectionUUIDString}`;

        return sourceCollection;
    }

    /**
     * Shards a non-existing collection using the specified shard key and chunk ranges.
     *
     * @param chunks - an array of
     * {min: <shardKeyValue0>, max: <shardKeyValue1>, shard: <shardName>} objects. The chunks must
     * form a partition of the {shardKey: MinKey} --> {shardKey: MaxKey} space.
     */
    createShardedCollection({
        ns,
        shardKeyPattern,
        chunks,
        primaryShardName = this.donorShardNames[0],
        collOptions = {},
    }) {
        this._ns = ns;
        this._currentShardKey = Object.assign({}, shardKeyPattern);

        const sourceCollection = this._st.s.getCollection(ns);
        const sourceDB = sourceCollection.getDB();

        // mongos won't know about the temporary resharding collection and will therefore assume the
        // collection is unsharded. We configure one of the recipient shards to be the primary shard
        // for the database so mongos still ends up routing operations to a shard which owns the
        // temporary resharding collection.
        assert.commandWorked(
            sourceDB.adminCommand({enableSharding: sourceDB.getName(), primaryShard: primaryShardName}),
        );
        this._primaryShardName = primaryShardName;

        CreateShardedCollectionUtil.shardCollectionWithChunks(sourceCollection, shardKeyPattern, chunks, collOptions);

        let tempCollNamePrefix = "system.resharding";
        // TODO SERVER-101784 simplify once only viewless timeseries collections exist.
        if (collOptions.timeseries !== undefined && !areViewlessTimeseriesEnabled(this._st.s)) {
            let bucketUUID = getUUIDFromListCollections(sourceDB, getTimeseriesBucketsColl(sourceCollection.getName()));

            assert.neq(bucketUUID, null, `can't find ns: ${this._ns} after creating chunks`);

            this._sourceCollectionUUID = bucketUUID;

            tempCollNamePrefix = getTimeseriesBucketsColl("resharding");
            this._underlyingSourceNs = `${sourceDB.getName()}.${getTimeseriesBucketsColl(sourceCollection.getName())}`;
        } else {
            this._sourceCollectionUUID = getUUIDFromListCollections(sourceDB, sourceCollection.getName());
            this._underlyingSourceNs = this._ns;
        }

        const sourceCollectionUUIDString = extractUUIDFromObject(this._sourceCollectionUUID);

        this._tempNs = `${sourceDB.getName()}.${tempCollNamePrefix}.${sourceCollectionUUIDString}`;

        return sourceCollection;
    }

    get tempNs() {
        assert.neq(undefined, this._tempNs, "createShardedCollection must be called first");
        return this._tempNs;
    }

    get presetReshardedChunks() {
        assert.neq(undefined, this._presetReshardedChunks, "createShardedCollection must be called first");
        return this._presetReshardedChunks;
    }

    get sourceCollectionUUID() {
        assert.neq(undefined, this._sourceCollectionUUID, "createShardedCollection must be called first");
        return this._sourceCollectionUUID;
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
        this._startReshardingInBackgroundAndAllowCommandFailure({newShardKeyPattern, newChunks}, ErrorCodes.OK);
    }

    /** @private */
    _startReshardingInBackgroundAndAllowCommandFailure(
        {newShardKeyPattern, newChunks, forceRedistribution, reshardingUUID, toShard, performVerification},
        expectedErrorCode,
    ) {
        for (let disallowedErrorCode of [ErrorCodes.FailedToSatisfyReadPreference, ErrorCodes.HostUnreachable]) {
            assert.neq(
                expectedErrorCode,
                disallowedErrorCode,
                `${ErrorCodeStrings[disallowedErrorCode]} error must never be expected as final` +
                    " reshardCollection command error response because it indicates mongos gave" +
                    " up retrying and the client must instead retry",
            );
        }

        this._newShardKey = Object.assign({}, newShardKeyPattern);

        if (toShard === undefined) {
            newChunks = newChunks.map((chunk) => ({min: chunk.min, max: chunk.max, recipientShardId: chunk.shard}));
            this._presetReshardedChunks = newChunks;
        }

        this._pauseCoordinatorBeforeBlockingWritesFailpoints = [];
        this._pauseCoordinatorBeforeDecisionPersistedFailpoints = [];
        this._pauseCoordinatorBeforeCompletionFailpoints = [];
        this._st.forEachConfigServer((configServer) => {
            this._pauseCoordinatorBeforeBlockingWritesFailpoints.push(
                configureFailPoint(configServer, "reshardingPauseCoordinatorBeforeBlockingWrites"),
            );
            this._pauseCoordinatorBeforeDecisionPersistedFailpoints.push(
                configureFailPoint(configServer, "reshardingPauseCoordinatorBeforeDecisionPersisted"),
            );
            this._pauseCoordinatorBeforeCompletionFailpoints.push(
                configureFailPoint(configServer, "reshardingPauseCoordinatorBeforeCompletion", {
                    "sourceNamespace": this._underlyingSourceNs,
                }),
            );
        });

        this._commandDoneSignal = new CountDownLatch(1);

        this._reshardingThread = new Thread(
            function (
                host,
                ns,
                newShardKeyPattern,
                newChunks,
                forceRedistribution,
                reshardingUUID,
                commandDoneSignal,
                opType,
                toShard,
                performVerification,
            ) {
                const conn = new Mongo(host);

                // We allow the client to retry the reshardCollection a large but still finite
                // number of times. This is done because the mongos would also return a
                // FailedToSatisfyReadPreference error response when the primary of the shard is
                // permanently down (e.g. due to a bug causing the server to crash) and it would be
                // preferable to not have the test run indefinitely in that situation.
                const kMaxNumAttempts = 40; // = [10 minutes / kDefaultFindHostTimeout]

                let res;
                for (let i = 1; i <= kMaxNumAttempts; ++i) {
                    let command = {};
                    switch (opType) {
                        case "reshardCollection":
                            command = {
                                reshardCollection: ns,
                                key: newShardKeyPattern,
                                _presetReshardedChunks: newChunks,
                            };
                            break;
                        case "moveCollection":
                            command = {moveCollection: ns};
                            break;
                        case "unshardCollection":
                            command = {unshardCollection: ns};
                            break;
                        default:
                            assert(false, "Unexpected opType");
                    }

                    if (forceRedistribution !== undefined) {
                        command = Object.merge(command, {forceRedistribution: forceRedistribution});
                    }
                    if (reshardingUUID !== undefined) {
                        // UUIDs are passed in as strings because the UUID type cannot pass
                        // through the thread constructor.
                        reshardingUUID = eval(reshardingUUID);
                        command = Object.merge(command, {reshardingUUID: reshardingUUID});
                    }
                    if (toShard !== undefined) {
                        command = Object.merge(command, {toShard: toShard});
                    }
                    if (performVerification !== undefined) {
                        command = Object.merge(command, {performVerification});
                    }
                    res = conn.adminCommand(command);

                    if (
                        res.ok === 1 ||
                        (res.code !== ErrorCodes.FailedToSatisfyReadPreference &&
                            res.code !== ErrorCodes.HostUnreachable)
                    ) {
                        commandDoneSignal.countDown();
                        break;
                    }

                    if (i < kMaxNumAttempts) {
                        print(
                            "Ignoring error from mongos giving up retrying" +
                                ` _shardsvrReshardCollection command: ${tojsononeline(res)}`,
                        );
                    }
                }

                return res;
            },
            this._st.s.host,
            this._ns,
            newShardKeyPattern,
            newChunks,
            forceRedistribution,
            reshardingUUID ? reshardingUUID.toString() : undefined,
            this._commandDoneSignal,
            this._opType,
            toShard,
            performVerification,
        );

        this._reshardingThread.start();
        this._isReshardingActive = true;
    }

    /**
     * Moves an existing unsharded collection to toShard.
     *
     * @param toShard - shardId of the shard to move to.
     *
     * @param duringReshardingFn - a function which optionally accepts the temporary resharding
     * namespace string. It is only guaranteed to be called after mongos has started running the
     * reshardCollection command. Callers should use DiscoverTopology.findConnectedNodes() to
     * introspect the state of the donor or recipient shards if they need more specific
     * synchronization.
     *
     * @param expectedErrorCode - the expected response code for the reshardCollection command.
     *
     * @param postCheckConsistencyFn - a function for evaluating additional correctness
     * assertions. This function is called in the critical section, after the `reshardCollection`
     * command has shuffled data, but before the coordinator persists a decision.
     *
     * @param postDecisionPersistedFn - a function for evaluating addition assertions after
     * the decision has been persisted, but before the resharding operation finishes and returns
     * to the client.
     *
     * @param afterReshardingFn - a function that will be called after the resharding operation
     * finishes but before checking the the state post resharding. By the time afterReshardingFn
     * is called the temporary resharding collection will either have been dropped or renamed.
     */
    withMoveCollectionInBackground(
        {toShard},
        duringReshardingFn = (tempNs) => {},
        {
            expectedErrorCode = ErrorCodes.OK,
            postCheckConsistencyFn = (tempNs) => {},
            postDecisionPersistedFn = () => {},
            afterReshardingFn = () => {},
        } = {},
    ) {
        this._opType = "moveCollection";
        this._startReshardingInBackgroundAndAllowCommandFailure(
            {newShardKeyPattern: {_id: 1}, toShard: toShard},
            expectedErrorCode,
        );

        assert.soon(() => {
            const op = this._findMoveCollectionCommandOp();
            return op !== undefined || this._commandDoneSignal.getCount() === 0;
        }, "failed to find moveCollection in $currentOp output");

        this._callFunctionSafely(() => duringReshardingFn(this._tempNs));
        this._checkConsistencyAndPostState(
            expectedErrorCode,
            () => postCheckConsistencyFn(this._tempNs),
            () => postDecisionPersistedFn(),
            () => afterReshardingFn(),
        );
    }

    /**
     * Unshards an existing sharded collection to toShard.
     *
     * @param toShard (Optional) - shardId of the shard to unshard to.
     *
     * @param duringReshardingFn - a function which optionally accepts the temporary resharding
     * namespace string. It is only guaranteed to be called after mongos has started running the
     * reshardCollection command. Callers should use DiscoverTopology.findConnectedNodes() to
     * introspect the state of the donor or recipient shards if they need more specific
     * synchronization.
     *
     * @param expectedErrorCode - the expected response code for the reshardCollection command.
     *
     * @param postCheckConsistencyFn - a function for evaluating additional correctness
     * assertions. This function is called in the critical section, after the `reshardCollection`
     * command has shuffled data, but before the coordinator persists a decision.
     *
     * @param postDecisionPersistedFn - a function for evaluating addition assertions after
     * the decision has been persisted, but before the resharding operation finishes and returns
     * to the client.
     *
     * @param afterReshardingFn - a function that will be called after the resharding operation
     * finishes but before checking the the state post resharding. By the time afterReshardingFn
     * is called the temporary resharding collection will either have been dropped or renamed.
     */
    withUnshardCollectionInBackground(
        {toShard},
        duringReshardingFn = (tempNs) => {},
        {
            expectedErrorCode = ErrorCodes.OK,
            postCheckConsistencyFn = (tempNs) => {},
            postDecisionPersistedFn = () => {},
            afterReshardingFn = () => {},
        } = {},
    ) {
        this._opType = "unshardCollection";
        this._startReshardingInBackgroundAndAllowCommandFailure(
            {newShardKeyPattern: {_id: 1}, toShard: toShard},
            expectedErrorCode,
        );

        assert.soon(() => {
            const op = this._findUnshardCollectionCommandOp();
            return op !== undefined || this._commandDoneSignal.getCount() === 0;
        }, "failed to find unshardCollection in $currentOp output");

        this._callFunctionSafely(() => duringReshardingFn(this._tempNs));
        this._checkConsistencyAndPostState(
            expectedErrorCode,
            () => postCheckConsistencyFn(this._tempNs),
            () => postDecisionPersistedFn(),
            () => afterReshardingFn(),
        );
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
     *
     * @param postCheckConsistencyFn - a function for evaluating additional correctness
     * assertions. This function is called in the critical section, after the `reshardCollection`
     * command has shuffled data, but before the coordinator persists a decision.
     *
     * @param postDecisionPersistedFn - a function for evaluating addition assertions after
     * the decision has been persisted, but before the resharding operation finishes and returns
     * to the client.
     *
     * @param afterReshardingFn - a function that will be called after the resharding operation
     * finishes but before checking the the state post resharding. By the time afterReshardingFn
     * is called the temporary resharding collection will either have been dropped or renamed.
     */
    withReshardingInBackground(
        {newShardKeyPattern, newChunks, forceRedistribution, reshardingUUID, performVerification},
        duringReshardingFn = (tempNs) => {},
        {
            expectedErrorCode = ErrorCodes.OK,
            postCheckConsistencyFn = (tempNs) => {},
            postDecisionPersistedFn = () => {},
            afterReshardingFn = () => {},
        } = {},
    ) {
        this._opType = "reshardCollection";
        this._startReshardingInBackgroundAndAllowCommandFailure(
            {
                newShardKeyPattern,
                newChunks,
                forceRedistribution,
                reshardingUUID,
                performVerification,
            },
            expectedErrorCode,
        );

        assert.soon(() => {
            const op = this._findReshardingCommandOp();
            return op !== undefined || this._commandDoneSignal.getCount() === 0;
        }, "failed to find reshardCollection in $currentOp output");

        this._callFunctionSafely(() => duringReshardingFn(this._tempNs));
        this._checkConsistencyAndPostState(
            expectedErrorCode,
            () => postCheckConsistencyFn(this._tempNs),
            () => postDecisionPersistedFn(),
            () => afterReshardingFn(),
        );
    }

    /** @private */
    _findMoveCollectionCommandOp() {
        const filter = {
            type: "op",
            "originatingCommand.reshardCollection": this._underlyingSourceNs,
            "provenance": "moveCollection",
        };

        return this._st.s
            .getDB("admin")
            .aggregate([{$currentOp: {allUsers: true, localOps: false}}, {$match: filter}])
            .toArray()[0];
    }

    /** @private */
    _findUnshardCollectionCommandOp() {
        const filter = {
            type: "op",
            "originatingCommand.reshardCollection": this._underlyingSourceNs,
            "provenance": "unshardCollection",
        };

        return this._st.s
            .getDB("admin")
            .aggregate([{$currentOp: {allUsers: true, localOps: false}}, {$match: filter}])
            .toArray()[0];
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
     * This helper attempts to disable the reshardingPauseCoordinatorBeforeBlockingWrites
     * failpoint when an exception is thrown to prevent the mongo shell from hanging (really the
     * config server) on top of having a JavaScript error.
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
            for (const fp of [
                ...this._pauseCoordinatorBeforeBlockingWritesFailpoints,
                ...this._pauseCoordinatorBeforeDecisionPersistedFailpoints,
                ...this._pauseCoordinatorBeforeCompletionFailpoints,
            ]) {
                try {
                    fp.off();
                } catch (disableFailpointError) {
                    print(
                        `Ignoring error from disabling the resharding coordinator failpoint: ${tojsononeline(
                            disableFailpointError,
                        )}`,
                    );

                    print(
                        "The config server primary and the mongo shell along with it are expected" +
                            " to hang due to the resharding coordinator being left uninterrupted",
                    );
                }
            }

            try {
                const op = this._findReshardingCommandOp();
                if (op !== undefined) {
                    assert.commandWorked(this._st.admin.killOp(op.opid));
                }

                try {
                    this._reshardingThread.join();
                } catch (joinError) {
                    print(`Ignoring error from the resharding thread: ${tojsononeline(joinError)}`);
                } finally {
                    print(
                        `Ignoring response from the resharding thread: ${tojsononeline(
                            this._reshardingThread.returnData(),
                        )}`,
                    );
                }

                this._isReshardingActive = false;
            } catch (killOpError) {
                print(
                    `Ignoring error from sending killOp to the reshardCollection command: ${tojsononeline(
                        killOpError,
                    )}`,
                );

                print("The mongo shell is expected to abort due to the resharding thread being" + " left unjoined");
            }

            throw duringReshardingError;
        }
    }

    interruptReshardingThread() {
        const op = this._findReshardingCommandOp();
        assert.neq(undefined, op, "failed to find reshardCollection in $currentOp output");
        assert.commandWorked(this._st.admin.killOp(op.opid));
    }

    /**
     * This method can be called with failpoints that block the `reshardCollection` command from
     * proceeding to the next stage. This helper returns after either:
     *
     * 1) The node's waitForFailPoint returns successfully or
     * 2) The `reshardCollection` command has returned a response or
     * 3) The ReshardingCoordinator is blocked on the reshardingPauseCoordinatorBeforeCompletion
     *    failpoint and won't ever satisfy the supplied failpoint.
     *
     * The function returns true when we returned because the server reached the failpoint. The
     * function returns false when the `reshardCollection` command is no longer running.
     * Otherwise the function throws an exception.
     *
     * @private
     */
    _waitForFailPoint(fp) {
        const completionFailpoint = this._pauseCoordinatorBeforeCompletionFailpoints.find(
            (completionFailpoint) => completionFailpoint.conn.host === fp.conn.host,
        );

        assert.soon(
            () => {
                if (this._commandDoneSignal.getCount() === 0 || fp.waitWithTimeout(1000)) {
                    return true;
                }

                if (completionFailpoint !== fp && completionFailpoint.waitWithTimeout(1000)) {
                    completionFailpoint.off();
                }

                return false;
            },
            "Timed out waiting for failpoint to be hit. Failpoint: " + fp.failPointName,
            undefined,
            // The `waitWithTimeout` command has the server block for an interval of time.
            1,
        );
        // return true if the `reshardCollection` command is still running.
        return this._commandDoneSignal.getCount() === 1;
    }

    /** @private */
    _checkConsistencyAndPostState(
        expectedErrorCode,
        postCheckConsistencyFn = () => {},
        postDecisionPersistedFn = () => {},
        afterReshardingFn = () => {},
    ) {
        // The CSRS primary may have changed as a result of running the duringReshardingFn()
        // callback function. The failpoints will only be triggered on the new CSRS primary so we
        // detect which node that is here.
        const configPrimary = this._st.configRS.getPrimary();
        const primaryIdx = this._pauseCoordinatorBeforeBlockingWritesFailpoints.findIndex(
            (fp) => fp.conn.host === configPrimary.host,
        );
        // The CSRS secondaries may be going through replication rollback which closes their
        // connections to the test client. We wait for any replication rollbacks to complete and for
        // the test client to have reconnected so the failpoints can be turned off on all of the
        // nodes later on.
        this._st.configRS.awaitSecondaryNodes();
        this._st.configRS.awaitReplication();

        let performCorrectnessChecks = true;
        if (expectedErrorCode === ErrorCodes.OK) {
            this._callFunctionSafely(() => {
                // We use the reshardingPauseCoordinatorBeforeBlockingWrites failpoint so that
                // any intervening writes performed on the sharded collection (from when the
                // resharding operation had started until now) are eventually applied by the
                // recipient shards. We then use the
                // reshardingPauseCoordinatorBeforeDecisionPersisted failpoint to wait for all of
                // the recipient shards to have applied through all of the oplog entries from all of
                // the donor shards.
                if (!this._waitForFailPoint(this._pauseCoordinatorBeforeBlockingWritesFailpoints[primaryIdx])) {
                    performCorrectnessChecks = false;
                }
                this._pauseCoordinatorBeforeBlockingWritesFailpoints.forEach((fp) => fp.off());

                // A resharding command that returned a failure will not hit the "Decision
                // Persisted" failpoint. If the command has returned, don't require that the
                // failpoint was entered. This ensures that following up by joining the
                // `_reshardingThread` will succeed.
                if (!this._waitForFailPoint(this._pauseCoordinatorBeforeDecisionPersistedFailpoints[primaryIdx])) {
                    performCorrectnessChecks = false;
                }

                // Don't correctness check the results if the resharding command unexpectedly
                // returned.
                if (performCorrectnessChecks) {
                    assert.commandWorked(this._st.s.adminCommand({flushRouterConfig: this._ns}));
                    this._checkConsistency();
                    this._checkDocumentOwnership();
                    postCheckConsistencyFn();
                }

                this._pauseCoordinatorBeforeDecisionPersistedFailpoints.forEach((fp) => fp.off());
                postDecisionPersistedFn();
                this._pauseCoordinatorBeforeCompletionFailpoints.forEach((fp) => fp.off());
            });
        } else {
            this._callFunctionSafely(() => {
                this._pauseCoordinatorBeforeBlockingWritesFailpoints.forEach((fp) =>
                    this.retryOnceOnNetworkError(fp.off),
                );
                postCheckConsistencyFn();
                this._pauseCoordinatorBeforeDecisionPersistedFailpoints.forEach((fp) =>
                    this.retryOnceOnNetworkError(fp.off),
                );

                postDecisionPersistedFn();
                this._pauseCoordinatorBeforeCompletionFailpoints.forEach((fp) => this.retryOnceOnNetworkError(fp.off));
            });
        }

        try {
            this._reshardingThread.join();
        } finally {
            this._isReshardingActive = false;
        }

        if (expectedErrorCode === ErrorCodes.OK) {
            assert.commandWorked(this._reshardingThread.returnData());
        } else {
            assert.commandFailedWithCode(this._reshardingThread.returnData(), expectedErrorCode);
        }

        // Reaching this line implies the `_reshardingThread` has successfully exited without
        // throwing an exception. Assert that we performed all expected correctness checks.
        assert(performCorrectnessChecks, {
            msg: "Reshard collection succeeded, but correctness checks were not performed.",
            expectedErrorCode: expectedErrorCode,
        });

        afterReshardingFn();
        this._checkPostState(expectedErrorCode);
    }

    /** @private */
    _checkConsistency() {
        // The "available" read concern level won't block this find cmd behind the critical section.
        // Tests for resharding are not expected to have unowned documents in the collection being
        // resharded.
        const nsCursor = this._st.s
            .getCollection(this._underlyingSourceNs)
            .find()
            .rawData()
            .readConcern("available")
            .sort({_id: 1});
        const tempNsCursor = this._st.s.getCollection(this._tempNs).find().rawData().sort({_id: 1});

        const diff = ((diff) => {
            return {
                docsWithDifferentContents: diff.docsWithDifferentContents.map(({first, second}) => ({
                    original: first,
                    resharded: second,
                })),
                docsExtraAfterResharding: diff.docsMissingOnFirst,
                docsMissingAfterResharding: diff.docsMissingOnSecond,
            };
        })(DataConsistencyChecker.getDiff(nsCursor, tempNsCursor));

        assert.eq(
            diff,
            {
                docsWithDifferentContents: [],
                docsExtraAfterResharding: [],
                docsMissingAfterResharding: [],
            },
            "existing sharded collection " +
                this._ns +
                " and temporary resharding collection " +
                this._tempNs +
                " had different" +
                " contents",
        );
    }

    /** @private */
    _checkDocumentOwnership() {
        // The "available" read concern level won't perform any ownership filtering. Any documents
        // which were copied by a recipient shard that are actually owned by a different recipient
        // shard would appear as extra documents.
        const tempColl = this._st.s.getCollection(this._tempNs);
        const localReadCursor = tempColl.find().rawData().sort({_id: 1});
        const availableReadCursor = tempColl.find().rawData().readConcern("available").sort({_id: 1});

        const diff = ((diff) => {
            return {
                docsWithDifferentContents: diff.docsWithDifferentContents.map(({first, second}) => ({
                    local: first,
                    available: second,
                })),
                docsFoundUnownedWithReadAvailable: diff.docsMissingOnFirst,
                docsNotFoundWithReadAvailable: diff.docsMissingOnSecond,
            };
        })(DataConsistencyChecker.getDiff(localReadCursor, availableReadCursor));

        assert.eq(
            diff,
            {
                docsWithDifferentContents: [],
                docsFoundUnownedWithReadAvailable: [],
                docsNotFoundWithReadAvailable: [],
            },
            "temporary resharding collection had unowned documents",
        );
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
        assert.eq(
            [],
            this._st.config.reshardingOperations
                .find({ns: this._underlyingSourceNs, state: {$ne: "quiesced"}})
                .toArray(),
            "expected config.reshardingOperations to be empty (except quiesced operations), but found it wasn't",
        );

        assert.eq(
            [],
            this._st.config.collections.find({reshardingFields: {$exists: true}}).toArray(),
            "expected there to be no config.collections entries with 'reshardingFields' set",
        );

        assert.eq(
            [],
            this._st.config.collections.find({allowMigrations: {$exists: true}}).toArray(),
            "expected there to be no config.collections entries with 'allowMigrations' set",
        );

        assert.eq(
            [],
            this._st.config.collections.find({_id: this._tempNs}).toArray(),
            "expected there to not be a config.collections entry for the temporary" + " resharding collection",
        );

        assert.eq(
            [],
            this._st.config.chunks.find({ns: this._tempNs}).toArray(),
            "expected there to not be any config.chunks entry for the temporary" + " resharding collection",
        );

        const collEntry = this._st.config.collections.findOne({_id: this._underlyingSourceNs});
        assert.neq(null, collEntry, `didn't find config.collections entry for ${this._underlyingSourceNs}`);

        if (expectedErrorCode === ErrorCodes.OK) {
            assert.eq(
                this._newShardKey,
                collEntry.key,
                "shard key pattern didn't change despite resharding having succeeded",
            );
            assert.neq(
                this._sourceCollectionUUID,
                collEntry.uuid,
                "collection UUID didn't change despite resharding having succeeded",
            );
        } else {
            assert.eq(
                this._currentShardKey,
                collEntry.key,
                "shard key pattern changed despite resharding having failed",
            );
            assert.eq(
                this._sourceCollectionUUID,
                collEntry.uuid,
                "collection UUID changed despite resharding having failed",
            );
        }
    }

    /** @private */
    _checkRecipientPostState(recipient, expectedErrorCode) {
        assert.eq(
            null,
            recipient.getCollection(this._tempNs).exists(),
            `expected the temporary resharding collection to not exist, but found it does on ${recipient.shardName}`,
        );

        const collInfo = recipient.getCollection(this._underlyingSourceNs).exists();
        const isAlsoDonor = this._donorShards().includes(recipient);
        if (expectedErrorCode === ErrorCodes.OK) {
            assert.neq(
                null,
                collInfo,
                `collection doesn't exist on ${recipient.shardName} despite resharding having succeeded`,
            );
            assert.neq(
                this._sourceCollectionUUID,
                collInfo.info.uuid,
                `collection UUID didn't change on ${recipient.shardName} despite resharding having succeeded`,
            );
        } else if (expectedErrorCode !== ErrorCodes.OK && !isAlsoDonor) {
            assert.eq(null, collInfo, `collection exists on ${recipient.shardName} despite resharding having failed`);
        }

        assert.eq(
            [],
            recipient.getCollection(`config.localReshardingOperations.recipient.progress_fetcher`).find().toArray(),
            `config.localReshardingOperations.recipient.progress_fetcher wasn't cleaned up on ${recipient.shardName}`,
        );

        assert.eq(
            [],
            recipient.getCollection(`config.localReshardingOperations.recipient.progress_applier`).find().toArray(),
            `config.localReshardingOperations.recipient.progress_applier wasn't cleaned up on ${recipient.shardName}`,
        );

        assert.eq(
            [],
            recipient.getCollection(`config.localReshardingOperations.recipient.progress_txn_cloner`).find().toArray(),
            `config.localReshardingOperations.recipient.progress_txn_cloner wasn't cleaned up on ${
                recipient.shardName
            }`,
        );

        const sourceCollectionUUIDString = extractUUIDFromObject(this._sourceCollectionUUID);
        for (const donor of this._donorShards()) {
            assert.eq(
                null,
                recipient
                    .getCollection(`config.localReshardingOplogBuffer.${sourceCollectionUUIDString}.${donor.shardName}`)
                    .exists(),
                `expected config.localReshardingOplogBuffer.${sourceCollectionUUIDString}.${
                    donor.shardName
                } not to exist on ${recipient.shardName}, but it did.`,
            );

            assert.eq(
                null,
                recipient
                    .getCollection(
                        `config.localReshardingConflictStash.${sourceCollectionUUIDString}.${donor.shardName}`,
                    )
                    .exists(),
                `expected config.localReshardingConflictStash.${sourceCollectionUUIDString}.${
                    donor.shardName
                } not to exist on ${recipient.shardName}, but it did.`,
            );
        }

        const localRecipientOpsNs = "config.localReshardingOperations.recipient";
        let res;
        assert.soon(
            () => {
                res = recipient.getCollection(localRecipientOpsNs).find().toArray();
                return res.length === 0;
            },
            () => `${localRecipientOpsNs} document wasn't cleaned up on ${recipient.shardName}: ${tojson(res)}`,
        );
    }

    /** @private */
    _checkDonorPostState(donor, expectedErrorCode) {
        const collInfo = donor.getCollection(this._underlyingSourceNs).exists();
        const isAlsoRecipient = this._recipientShards().includes(donor) || donor.shardName === this._primaryShardName;
        if (expectedErrorCode === ErrorCodes.OK && !isAlsoRecipient) {
            assert(collInfo == null, `collection exists on ${donor.shardName} despite resharding having succeeded`);
        } else if (expectedErrorCode !== ErrorCodes.OK) {
            assert.neq(
                null,
                collInfo,
                `collection doesn't exist on ${donor.shardName} despite resharding having failed`,
            );
            assert.eq(
                this._sourceCollectionUUID,
                collInfo.info.uuid,
                `collection UUID changed on ${donor.shardName} despite resharding having failed`,
            );
        }

        const localDonorOpsNs = "config.localReshardingOperations.donor";
        let res;
        assert.soon(
            () => {
                res = donor.getCollection(localDonorOpsNs).find().toArray();
                return res.length === 0;
            },
            () => `${localDonorOpsNs} document wasn't cleaned up on ${donor.shardName}: ${tojson(res)}`,
        );
    }

    teardown() {
        if (this._isReshardingActive) {
            this._checkConsistencyAndPostState(ErrorCodes.OK);
        }

        this._st.stop();
    }

    /**
     * Given the shardName, steps up a secondary (chosen at random) to become the new primary of the
     * shard replica set. To force an election on the configsvr rather than a participant shard, use
     * shardName = this.configShardName;
     */
    stepUpNewPrimaryOnShard(shardName) {
        jsTestLog(`ReshardingTestFixture stepping up new primary on shard ${shardName}`);

        const replSet = this.getReplSetForShard(shardName);
        let originalPrimary = replSet.getPrimary();
        let secondaries = replSet.getSecondaries();

        while (secondaries.length > 0) {
            // Once the primary is terminated/killed/stepped down, write availability is lost. Avoid
            // long periods where the replica set doesn't have write availability by trying to step
            // up secondaries until one succeeds.
            const newPrimaryIdx = Random.randInt(secondaries.length);
            const newPrimary = secondaries[newPrimaryIdx];

            let res;
            try {
                res = newPrimary.adminCommand({replSetStepUp: 1});
            } catch (e) {
                if (!isNetworkError(e)) {
                    throw e;
                }

                jsTest.log(
                    `ReshardingTestFixture got a network error ${tojson(e)} while` +
                        ` attempting to step up secondary ${newPrimary.host}. This is likely due to` +
                        ` the secondary previously having transitioned through ROLLBACK and closing` +
                        ` its user connections. Will retry stepping up the same secondary again`,
                );
                res = newPrimary.adminCommand({replSetStepUp: 1});
            }

            if (res.ok === 1) {
                replSet.awaitNodesAgreeOnPrimary();
                assert.eq(newPrimary, replSet.getPrimary());
                this._st.getAllNodes().forEach((conn) => {
                    awaitRSClientHosts(conn, {host: newPrimary.host}, {ok: true, ismaster: true});
                });
                return;
            }

            jsTest.log(
                `ReshardingTestFixture failed to step up secondary ${newPrimary.host} and` +
                    ` got error ${tojson(res)}. Will retry on another secondary until all` +
                    ` secondaries have been exhausted`,
            );
            secondaries.splice(newPrimaryIdx, 1);
        }

        jsTest.log(`ReshardingTestFixture failed to step up secondaries, trying to step` + ` original primary back up`);
        replSet.stepUp(originalPrimary, {awaitReplicationBeforeStepUp: false});
    }

    killAndRestartPrimaryOnShard(shardName) {
        jsTest.log(`ReshardingTestFixture killing and restarting primary on shard ${shardName}`);
        const replSet = this.getReplSetForShard(shardName);

        this._st.killAndRestartPrimaryOnShard(shardName, replSet);

        const newPrimaryConn = replSet.getPrimary();
        this._st.getAllNodes().forEach((conn) => {
            awaitRSClientHosts(conn, {host: newPrimaryConn.host}, {ok: true, ismaster: true});
        });
    }

    shutdownAndRestartPrimaryOnShard(shardName) {
        jsTest.log(`ReshardingTestFixture shutting down and restarting primary on shard ${shardName}`);
        const replSet = this.getReplSetForShard(shardName);

        this._st.shutdownAndRestartPrimaryOnShard(shardName, replSet);

        const newPrimaryConn = replSet.getPrimary();
        this._st.getAllNodes().forEach((conn) => {
            awaitRSClientHosts(conn, {host: newPrimaryConn.host}, {ok: true, ismaster: true});
        });
    }

    updatePrimaryShard(primaryShardName) {
        jsTest.log(`ReshardingTestFixture updating primary shard to ${primaryShardName}`);
        this._primaryShardName = primaryShardName;
    }

    /**
     * @returns the timestamp chosen by the resharding operation for cloning.
     *
     * Should also be used in tandem with retryableWriteManager when calling this method in a
     * jstestfuzzer code for backwards compatibility, like so:
     *
     * if (reshardingTest.awaitCloneTimestampChosen !== undefined) {
     *     fetchTimestamp = reshardingTest.awaitCloneTimestampChosen();
     * } else {
     *     fetchTimestamp = retryableWriteManager.awaitFetchTimestampChosen();
     * }
     */
    awaitCloneTimestampChosen() {
        let cloneTimestamp;

        assert.soon(() => {
            const coordinatorDoc = this._st.config.reshardingOperations.findOne({ns: this._underlyingSourceNs});
            cloneTimestamp = coordinatorDoc !== null ? coordinatorDoc.cloneTimestamp : undefined;
            return cloneTimestamp !== undefined;
        });

        for (let donor of this._donorShards()) {
            // Send a command through the router so $clusterTime gossiping advances the timestamp to
            // atleast the cloneTimestamp on all donors.
            donor.getCollection(this._underlyingSourceNs).findOne();
        }

        return cloneTimestamp;
    }

    /**
     * Calls and returns the value from the supplied function.
     *
     * If a network error is thrown during its execution, then this function will invoke the
     * supplied function a second time. This pattern is useful for tolerating network errors which
     * result from elections triggered by any of the stepUpNewPrimaryOnShard(),
     * killAndRestartPrimaryOnShard(), and shutdownAndRestartPrimaryOnShard() methods.
     *
     * @param fn - the function to be called.
     * @returns the return value from fn.
     */
    retryOnceOnNetworkError(fn) {
        try {
            return fn();
        } catch (e) {
            if (!isNetworkError(e)) {
                throw e;
            }

            return fn();
        }
    }
};
