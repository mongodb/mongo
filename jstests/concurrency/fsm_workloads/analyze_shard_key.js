'use strict';

/**
 * Tests that the analyzeShardKey command returns correct metrics.
 *
 * This workload implicitly assumes that its tid range is [0, $config.threadCount). This isn't
 * guaranteed to be true when it is run in parallel with other workloads.
 *
 * TODO (SERVER-75532): Investigate the high variability of the runtime of analyze_shard_key.js in
 * suites with chunk migration and/or stepdown/kill/terminate.
 * @tags: [
 *  requires_fcv_70,
 *  featureFlagAnalyzeShardKey,
 *  featureFlagUpdateOneWithoutShardKey,
 *  uses_transactions,
 *  resource_intensive,
 *  incompatible_with_concurrency_simultaneous,
 *  does_not_support_stepdowns,
 *  assumes_balancer_off
 * ]
 */
load("jstests/concurrency/fsm_libs/extend_workload.js");
load("jstests/concurrency/fsm_workload_helpers/server_types.js");  // for isMongos
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'
load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");

const aggregateInterruptErrors =
    [ErrorCodes.CursorNotFound, ErrorCodes.CursorKilled, ErrorCodes.QueryPlanKilled];

if ($config === undefined) {
    // There is no workload to extend. Define a noop base workload to make the 'extendWorkload' call
    // below still work.
    $config = {
        threadCount: 1,
        iterations: 1,
        startState: "init",
        data: {},
        states: {init: function(db, collName) {}},
        transitions: {init: {init: 1}},
        setup: function(db, collName) {},
        teardown: function(db, collName) {},
    };
}

var $config = extendWorkload($config, function($config, $super) {
    $config.threadCount = 10;
    $config.iterations = 500;

    // The sample rate range for query sampling.
    $config.data.minSampleRate = 1000;
    $config.data.maxSampleRate = 1500;
    // The comment to attached to queries in the read and write states below to mark them as
    // eligible for sampling. Queries such as the aggregate queries for looking up documents to
    // update will not have this comment attached since they do not follow the query patterns
    // defined for the workload so can cause the read distribution metrics to be incorrect.
    $config.data.eligibleForSamplingComment = "eligible for query sampling";

    // The fields in the documents in the test collection. The unique document id is is used in
    // write filters to make each write only modify or delete one document whether it specifies
    // "multi" true or false. This is to avoid drastically changing the cardinality and
    // frequency of the shard key as this workload runs.
    $config.data.idFieldName = "uid";
    $config.data.candidateShardKeyFieldName = "candidateKeyField";
    $config.data.nonCandidateShardKeyFieldName = "nonCandidateKeyField";
    // The shard key field if the workload is running on a sharded cluster.
    $config.data.currentShardKeyFieldName = "currentKeyField";
    $config.data.shardKey = {[$config.data.currentShardKeyFieldName]: "hashed"};

    // The settings for generating the initial documents if the shard key is unique.
    $config.data.uniqueShardKeyOptions = {
        minInitialNumDocs: 15000,
        maxInitialNumDocs: 20000,
    };
    // The settings for generating the initial documents if the shard key in not unique.
    $config.data.nonUniqueShardKeyOptions = {
        minInitialNumDistinctValues: 1500,
        maxInitialNumDistinctValues: 2000,
        minInitialFrequency: 10,
        maxInitialFrequency: 15,
    };

    ////
    // The helpers for setting up the test collection.

    /**
     * Generates the shard key to be analyzed.
     */
    $config.data.generateShardKeyOptions = function generateShardKeyOptions(cluster) {
        const isHashed = Math.random() < 0.5;
        const isUnique = Math.random() < 0.5;
        const isMonotonic = Math.random() < 0.5;

        let shardKey, indexSpecs;
        if (cluster.isSharded() && isUnique) {
            // It is illegal to create a unique index that doesn't have the shard key as a prefix.
            shardKey = {
                [this.currentShardKeyFieldName]: isHashed ? "hashed" : 1,
                [this.candidateShardKeyFieldName]: 1
            };
            indexSpecs = [{
                name: "compatible_index",
                key: {[this.currentShardKeyFieldName]: 1, [this.candidateShardKeyFieldName]: 1},
                unique: isUnique
            }];
            // For a compound hashed shard key, the exact shard key index is needed for determining
            // the monotonicity.
            if (isHashed) {
                indexSpecs.push({name: "exact_index", key: shardKey});
            }
        } else {
            shardKey = {[this.candidateShardKeyFieldName]: isHashed ? "hashed" : 1};
            indexSpecs = [{
                name: "compatible_index",
                key: {[this.candidateShardKeyFieldName]: 1},
                unique: isUnique
            }];
        }
        const isCurrentShardKey =
            cluster.isSharded() && bsonWoCompare(this.shardKey, shardKey) === 0;

        this.shardKeyOptions =
            {isHashed, isUnique, isMonotonic, isCurrentShardKey, shardKey, indexSpecs};
    };

    /**
     * Generates the document schema for the collection. Specifically, it defines the type and
     * monotonicity of each field.
     */
    $config.data.generateDocumentOptions = function generateDocumentOptions(cluster) {
        this.documentOptions = {};
        for (let fieldName in this.shardKeyOptions.shardKey) {
            this.documentOptions[fieldName] = {
                isMonotonic: this.shardKeyOptions.isMonotonic,
                type: this.shardKeyOptions.isMonotonic ? "objectId" : "uuid"
            };
        }
        this.documentOptions[this.nonCandidateShardKeyFieldName] = {
            isMonotonic: false,
            type: "uuid"
        };
        if (cluster.isSharded() &&
            !this.documentOptions.hasOwnProperty(this.currentShardKeyFieldName)) {
            this.documentOptions[this.currentShardKeyFieldName] = {
                isMonotonic: false,
                type: "uuid"
            };
        }
    };

    /**
     * Generates a unique value for the field with the given name.
     */
    $config.data.generateRandomValue = function generateRandomValue(fieldName) {
        const fieldType = this.documentOptions[fieldName].type;
        if (fieldType == "objectId") {
            return new ObjectId();
        } else if (fieldType == "uuid") {
            return new UUID();
        }
        throw new Error("Unknown field type");
    };

    /**
     * Generates a document for the thread with the given id such that the value of every field in
     * the document is unique. Does not assign a unique id to the document.
     */
    $config.data.generateRandomDocumentBase = function generateRandomDocumentBase(tid) {
        let doc = {tid};
        for (let fieldName in this.documentOptions) {
            doc[fieldName] = this.generateRandomValue(fieldName);
        }
        return doc;
    };

    /**
     * Same as above but assigns a unique id to the document.
     */
    $config.data.generateRandomDocument = function generateRandomDocument(tid) {
        const docBase = this.generateRandomDocumentBase(tid);
        return Object.assign(docBase, {[this.idFieldName]: UUID()});
    };

    /**
     * Returns a random document assigned to the thread invoking this.
     */
    $config.data.getRandomDocument = function getRandomDocument(db, collName) {
        const res = assert.commandWorked(db.runCommand({
            aggregate: collName,
            pipeline: [{$match: {tid: this.tid}}, {$sample: {size: 1}}],
            cursor: {}
        }));
        assert.eq(res.cursor.id, 0, res);
        return res.cursor.firstBatch[0];
    };

    /**
     * Generates and inserts initial documents.
     */
    $config.data.generateInitialDocuments = function generateInitialDocuments(
        db, collName, cluster) {
        this.numInitialDocuments = 0;
        this.numInitialDistinctValues = 0;
        let docs = [];

        if (this.shardKeyOptions.isUnique) {
            this.numInitialDocuments =
                AnalyzeShardKeyUtil.getRandInteger(this.uniqueShardKeyOptions.minInitialNumDocs,
                                                   this.uniqueShardKeyOptions.maxInitialNumDocs);
            this.numInitialDistinctValues = this.numInitialDocuments;
            for (let i = 0; i < this.numInitialDocuments; ++i) {
                const docBase = this.generateRandomDocumentBase(i % this.threadCount);
                docs.push(Object.assign(docBase, {[this.idFieldName]: UUID()}));
            }
        } else {
            this.numInitialDistinctValues = AnalyzeShardKeyUtil.getRandInteger(
                this.nonUniqueShardKeyOptions.minInitialNumDistinctValues,
                this.nonUniqueShardKeyOptions.maxInitialNumDistinctValues);
            for (let i = 0; i < this.numInitialDistinctValues; ++i) {
                const docBase = this.generateRandomDocumentBase(i % this.threadCount);
                const frequency = AnalyzeShardKeyUtil.getRandInteger(
                    this.nonUniqueShardKeyOptions.minInitialFrequency,
                    this.nonUniqueShardKeyOptions.maxInitialFrequency);
                for (let j = 0; j < frequency; j++) {
                    docs.push(Object.assign({}, docBase, {[this.idFieldName]: UUID()}));
                    this.numInitialDocuments++;
                }
            }
        }
        assert.eq(docs.length, this.numInitialDocuments);

        assert.commandWorked(
            db.runCommand({createIndexes: collName, indexes: this.shardKeyOptions.indexSpecs}));
        assert.commandWorked(db.runCommand({insert: collName, documents: docs}));

        // Wait for the documents to get replicated to all nodes so that a analyzeShardKey command
        // runs immediately after this can assert on the metrics regardless of which nodes it
        // targets.
        cluster.awaitReplication();

        print(`Set up collection that have the following shard key to analyze ${tojson({
            shardKeyOptions: this.shardKeyOptions,
            documentOptions: this.documentOptions,
            numInitialDocuments: this.numInitialDocuments,
            numInitialDistinctValues: this.numInitialDistinctValues
        })}`);
    };

    ////
    // The helpers for setting up the query patterns.

    /**
     * Generates n non-negative numbers whose sum is 100.
     */
    $config.data.generateRandomPercentages = function generateRandomPercentages(n) {
        const rands = Array.from({length: n}, () => Math.random());
        const sum = rands.reduce((partialSum, x) => partialSum + x, 0);
        return rands.map(rand => rand * 100 / sum);
    };

    /**
     * Calculates the shard targeting metrics based on the given information about query patterns
     * and the shard key being analyzed.
     */
    $config.data.calculateShardTargetingMetrics = function calculateShardTargetingMetrics(
        percentageOfFilterByShardKeyEquality,
        percentageOfFilterByShardKeyRange,
        percentageOfNotFilterByShardKey) {
        const totalPercentage = percentageOfFilterByShardKeyEquality +
            percentageOfFilterByShardKeyRange + percentageOfNotFilterByShardKey;
        AnalyzeShardKeyUtil.assertApprox(totalPercentage, 100);

        const percentageOfSingleShard = percentageOfFilterByShardKeyEquality;
        const percentageOfMultiShard =
            this.shardKeyOptions.isHashed ? 0 : percentageOfFilterByShardKeyRange;
        const percentageOfScatterGather = this.shardKeyOptions.isHashed
            ? (percentageOfFilterByShardKeyRange + percentageOfNotFilterByShardKey)
            : percentageOfNotFilterByShardKey;

        return [percentageOfSingleShard, percentageOfMultiShard, percentageOfScatterGather];
    };

    /**
     * Generates query patterns for this workload. Specifically, it defines the percentages of
     * reads/writes that filter by shard key equality, filter by shard key range and do not filter
     * by shard key at all, and the percentages of single writes and multi writes, and percentage
     * of shard key updates. Then, it calculates the ideal read and write distribution metrics that
     * the analyzeShardKey command should return.
     */
    $config.data.generateRandomQueryPatterns = function generateRandomQueryPatterns() {
        [this.percentageOfReadsFilterByShardKeyEquality,
         this.percentageOfReadsFilterByShardKeyRange,
         this.percentageOfReadsNotFilterByShardKey] = this.generateRandomPercentages(3);
        const [percentageOfSingleShardReads,
               percentageOfMultiShardReads,
               percentageOfScatterGatherReads] =
            this.calculateShardTargetingMetrics(this.percentageOfReadsFilterByShardKeyEquality,
                                                this.percentageOfReadsFilterByShardKeyRange,
                                                this.percentageOfReadsNotFilterByShardKey);
        this.readDistribution = {
            percentageOfSingleShardReads,
            percentageOfMultiShardReads,
            percentageOfScatterGatherReads
        };

        [this.percentageOfWritesFilterByShardKeyEquality,
         this.percentageOfWritesFilterByShardKeyRange,
         this.percentageOfWritesNotFilterByShardKey] = this.generateRandomPercentages(3);
        const [percentageOfSingleShardWrites,
               percentageOfMultiShardWrites,
               percentageOfScatterGatherWrites] =
            this.calculateShardTargetingMetrics(this.percentageOfWritesFilterByShardKeyEquality,
                                                this.percentageOfWritesFilterByShardKeyRange,
                                                this.percentageOfWritesNotFilterByShardKey);

        this.probabilityOfMultiWrites = (() => {
            if (TestData.runningWithShardStepdowns) {
                // Multi-writes are not retryable.
                return 0;
            }
            if (TestData.runningWithBalancer) {
                // Multi-writes against a sharded collection do not synchronize with chunk
                // migrations or perform shard filtering. As a result, they can end up getting
                // applied to the same document between zero and multiple times.
                return 0;
            }
            return Math.random();
        })();
        // The write states "update", "remove", "findAndModifyUpdate" and "findAndModifyRemove"
        // all have equal incoming state transition probabilities and the multi writes are not
        // applicable to the "findAndModify" states.
        this.percentageOfSingleWrites = 50 + (1 - this.probabilityOfMultiWrites) * 50;
        this.percentageOfMultiWrites = this.probabilityOfMultiWrites * 50;

        const percentageOfWritesWithoutShardKey = this.percentageOfWritesFilterByShardKeyRange +
            this.percentageOfWritesNotFilterByShardKey;
        const percentageOfSingleWritesWithoutShardKey =
            this.percentageOfSingleWrites * percentageOfWritesWithoutShardKey / 100;
        const percentageOfMultiWritesWithoutShardKey =
            this.percentageOfMultiWrites * percentageOfWritesWithoutShardKey / 100;

        this.probabilityOfShardKeyUpdates = Math.random();
        // The write states "update", "remove", "findAndModifyUpdate" and "findAndModifyRemove"
        // all have equal incoming state transition probabilities and only updates are not
        // applicable to the "remove" and "findAndModifyRemove" states.
        const percentageOfShardKeyUpdates = this.probabilityOfShardKeyUpdates * 50;

        this.writeDistribution = {
            percentageOfSingleShardWrites,
            percentageOfMultiShardWrites,
            percentageOfScatterGatherWrites,
            percentageOfShardKeyUpdates,
            percentageOfSingleWritesWithoutShardKey,
            percentageOfMultiWritesWithoutShardKey,
        };

        print(`Testing the following read and write distribution ${tojson({
            readQueryPatterns: {
                percentageFilterByShardKeyEquality: this.percentageOfReadsFilterByShardKeyEquality,
                percentageFilterByShardKeyRange: this.percentageOfReadsFilterByShardKeyRange,
                percentageNotFilterByShardKey: this.percentageOfReadsNotFilterByShardKey
            },
            writeQueryPatterns: {
                percentageFilterByShardKeyEquality: this.percentageOfWritesFilterByShardKeyEquality,
                percentageFilterByShardKeyRange: this.percentageOfWritesFilterByShardKeyRange,
                percentageNotFilterByShardKey: this.percentageOfWritesNotFilterByShardKey
            },
            readDistribution: this.readDistribution,
            writeDistribution: this.writeDistribution,
        })}`);
    };

    ////
    // The helpers for generating queries.

    /**
     * Generates a read filter.
     */
    $config.data.generateRandomReadFilter = function generateRandomReadFilter() {
        const rand = Math.random() * 100;
        if (rand < this.percentageOfReadsFilterByShardKeyEquality) {
            let filter = {};
            for (let fieldName in this.shardKeyOptions.shardKey) {
                filter[fieldName] = this.generateRandomValue(fieldName);
            }
            return filter;
        }
        if (rand < (this.percentageOfReadsFilterByShardKeyEquality +
                    this.percentageOfReadsFilterByShardKeyRange)) {
            let filter = {};
            for (let fieldName in this.shardKeyOptions.shardKey) {
                filter[fieldName] = {$gte: this.generateRandomValue(fieldName)};
            }
            return filter;
        }
        return {
            [this.nonCandidateShardKeyFieldName]:
                this.generateRandomValue(this.nonCandidateShardKeyFieldName)
        };
    };

    /**
     * Generates a write filter that will match exactly one document assigned to the thread invoking
     * this. If the document lookup fails with an expected error, returns null. If it fails with
     * some other error, throws the error.
     */
    $config.data.tryGenerateRandomWriteFilter = function tryGenerateRandomWriteFilter(db,
                                                                                      collName) {
        let doc;
        try {
            doc = this.getRandomDocument(db, collName);
        } catch (e) {
            if (this.isAcceptableDocumentLookUpError(e)) {
                return null;
            }
            throw e;
        }
        assert.eq(doc.tid, this.tid, doc);

        // Specify the unique document id in the filter so that there is only one matching document.
        let filter = {tid: this.tid, [this.idFieldName]: doc[this.idFieldName]};

        const rand = Math.random() * 100;
        if (rand < this.percentageOfWritesFilterByShardKeyEquality) {
            for (let fieldName in this.shardKeyOptions.shardKey) {
                filter[fieldName] = doc[fieldName];
            }
        } else if (rand < (this.percentageOfWritesFilterByShardKeyEquality +
                           this.percentageOfWritesFilterByShardKeyRange)) {
            for (let fieldName in this.shardKeyOptions.shardKey) {
                filter[fieldName] = {$gte: doc[fieldName]};
            }
        } else {
            filter[this.nonCandidateShardKeyFieldName] = doc[this.nonCandidateShardKeyFieldName];
        }

        return filter;
    };

    /**
     * Generates a modifier update which sets the value of the field being modified to a new unique
     * value.
     */
    $config.data.generateRandomModifierUpdate = function generateRandomModifierUpdate() {
        const fieldName = Math.random() < this.probabilityOfShardKeyUpdates
            ? this.candidateShardKeyFieldName
            : this.nonCandidateShardKeyFieldName;
        return {$set: {[fieldName]: this.generateRandomValue(fieldName)}};
    };

    ////
    // The helpers for verifying the metrics returned by the analyzeShardKey command.

    // The name of the collection used for storing the latest metrics by returned the
    // analyzeShardKey command. The read and write metrics are validated with a more narrow diff
    // window during teardown.
    $config.data.metricsCollName = "analyzeShardKeyMetrics";

    // The diff window for the metrics about the characteristics of the shard key.
    $config.data.numDistinctValuesMaxDiffPercentage = 1;

    /**
     * Verifies that the metrics about the characteristics of the shard key are within acceptable
     * ranges.
     */
    $config.data.assertKeyCharacteristicsMetrics = function assertKeyCharacteristicsMetrics(
        metrics) {
        // Perform basic validation of the metrics.
        AnalyzeShardKeyUtil.assertContainKeyCharacteristicsMetrics(metrics);
        assert.eq(metrics.isUnique, this.shardKeyOptions.isUnique, metrics);

        // Validate the cardinality metrics. Due to the concurrent writes by other threads, it is
        // not feasible to assert on the exact "numDistinctValues" value. However, given that this
        // workload inserts a new document every time it removes a document and that it generates a
        // new value every time it updates a document, "numDistinctValues" should be greater or
        // equal to the initial number of distinct values except in the following cases:
        // 1. The collection has gone through migrations.
        // 2. There have been unclean shutdowns (i.e. kills).
        // The reason is that the cardinality metrics are calculated as follows. If the shard key is
        // not unique, they are calculated using an aggregation with readConcern "available" (i.e.
        // it opts out of shard versioning and filtering). If the shard key is unique, they are
        // inferred from fast count of the documents.
        if (metrics.numDistinctValues < this.numInitialDistinctValues) {
            if (!TestData.runningWithBalancer) {
                assert(this.shardKeyOptions.isUnique, metrics);
                if (!TestData.runningWithShardStepdowns) {
                    // If there are no unclean shutdowns, the diff should be negligible.
                    AnalyzeShardKeyUtil.assertDiffPercentage(
                        metrics.numDistinctValues,
                        this.numInitialDistinctValues,
                        this.numDistinctValuesMaxDiffPercentage);
                }
            }
        }

        // Validate the frequency metrics. Likewise, due to the concurrent writes by other threads,
        // it is not feasible to assert on the exact "mostCommonValues".
        assert.eq(metrics.mostCommonValues.length, this.analyzeShardKeyNumMostCommonValues);

        // Validate the monotonicity metrics. This check is skipped if the balancer is enabled
        // since chunk migration deletes documents from the donor shard and re-inserts them on the
        // recipient shard so there is no guarantee that the insertion order from the client is
        // preserved.
        if (!TestData.runningWithBalancer) {
            assert.eq(metrics.monotonicity.type,
                      this.shardKeyOptions.isMonotonic && !this.shardKeyOptions.isHashed
                          ? "monotonic"
                          : "not monotonic",
                      metrics.monotonicity);
        }
    };

    // The intermediate and final diff windows for the metrics about the read and write
    // distribution.
    $config.data.intermediateReadDistributionMetricsMaxDiff = 20;
    $config.data.intermediateWriteDistributionMetricsMaxDiff = 20;
    // The final diff windows are larger when the reads and writes are run inside transactions or
    // with stepdown/kill/terminate in the background due to the presence of retries from the
    // external client.
    $config.data.finalReadDistributionMetricsMaxDiff =
        (TestData.runInsideTransaction || TestData.runningWithShardStepdowns) ? 15 : 12;
    $config.data.finalWriteDistributionMetricsMaxDiff =
        (TestData.runInsideTransaction || TestData.runningWithShardStepdowns) ? 15 : 12;
    // The minimum number of sampled queries to wait for before verifying the read and write
    // distribution metrics.
    $config.data.numSampledQueriesThreshold = 1500;

    /**
     * Verifies that the metrics about the read and write distribution are within acceptable ranges.
     */
    $config.data.assertReadWriteDistributionMetrics = function assertReadWriteDistributionMetrics(
        metrics, isFinal) {
        AnalyzeShardKeyUtil.assertContainReadWriteDistributionMetrics(metrics);

        let assertReadMetricsDiff = (actual, expected) => {
            const maxDiff = isFinal ? this.finalReadDistributionMetricsMaxDiff
                                    : this.intermediateReadDistributionMetricsMaxDiff;
            assert.lt(Math.abs(actual - expected), maxDiff, {actual, expected});
        };
        let assertWriteMetricsDiff = (actual, expected) => {
            const maxDiff = isFinal ? this.finalWriteDistributionMetricsMaxDiff
                                    : this.intermediateWriteDistributionMetricsMaxDiff;
            assert.lt(Math.abs(actual - expected), maxDiff, {actual, expected});
        };

        if (metrics.readDistribution.sampleSize.total > this.numSampledQueriesThreshold) {
            assertReadMetricsDiff(metrics.readDistribution.percentageOfSingleShardReads,
                                  this.readDistribution.percentageOfSingleShardReads);
            assertReadMetricsDiff(metrics.readDistribution.percentageOfMultiShardReads,
                                  this.readDistribution.percentageOfMultiShardReads);
            assertReadMetricsDiff(metrics.readDistribution.percentageOfScatterGatherReads,
                                  this.readDistribution.percentageOfScatterGatherReads);
            assert.eq(metrics.readDistribution.numReadsByRange.length,
                      this.analyzeShardKeyNumRanges);
        }
        if (metrics.writeDistribution.sampleSize.total > this.numSampledQueriesThreshold) {
            assertWriteMetricsDiff(metrics.writeDistribution.percentageOfSingleShardWrites,
                                   this.writeDistribution.percentageOfSingleShardWrites);
            assertWriteMetricsDiff(metrics.writeDistribution.percentageOfMultiShardWrites,
                                   this.writeDistribution.percentageOfMultiShardWrites);
            assertWriteMetricsDiff(metrics.writeDistribution.percentageOfScatterGatherWrites,
                                   this.writeDistribution.percentageOfScatterGatherWrites);
            assertWriteMetricsDiff(metrics.writeDistribution.percentageOfShardKeyUpdates,
                                   this.writeDistribution.percentageOfShardKeyUpdates);
            assertWriteMetricsDiff(
                metrics.writeDistribution.percentageOfSingleWritesWithoutShardKey,
                this.writeDistribution.percentageOfSingleWritesWithoutShardKey);
            assertWriteMetricsDiff(metrics.writeDistribution.percentageOfMultiWritesWithoutShardKey,
                                   this.writeDistribution.percentageOfMultiWritesWithoutShardKey);
            assert.eq(metrics.writeDistribution.numWritesByRange.length,
                      this.analyzeShardKeyNumRanges);
        }
    };

    ////
    // The helpers for handling errors.

    $config.data.isAcceptableAnalyzeShardKeyError = function isAcceptableAnalyzeShardKeyError(err) {
        if (err.code == ErrorCodes.StaleConfig && TestData.runningWithBalancer) {
            // Due to the size of the collection, each chunk migration can take quite some time to
            // complete. For the analyzeShardKey command, it turns out mongos can sometimes use up
            // all of its StaleConfig retries. This is likely caused by the refreshes that occur as
            // metrics are calculated.
            print(`Failed to analyze the shard key due to a stale config error ${
                tojsononeline(err)}`);
            return true;
        }
        if (err.code == 28799 || err.code == 4952606) {
            // (WT-8003) 28799 is the error that $sample throws when it fails to find a
            // non-duplicate document using a random cursor. 4952606 is the error that the sampling
            // based split policy throws if it fails to find the specified number of split points.
            print(
                `Failed to analyze the shard key due to duplicate keys returned by random cursor ${
                    tojsononeline(err)}`);
            return true;
        }
        if (this.expectedAggregateInterruptErrors.includes(err.code)) {
            print(
                `Failed to analyze the shard key because internal aggregate commands got interrupted ${
                    tojsononeline(err)}`);
            return true;
        }
        return false;
    };

    $config.data.isAcceptableUpdateError = function isAcceptableUpdateError(err) {
        if (err.code === ErrorCodes.DuplicateKey) {
            // The duplicate key error is only acceptable if it's a document shard key update during
            // a migration.
            return err.errmsg.includes("Failed to update document's shard key field");
        }
        return false;
    };

    $config.data.isAcceptableDocumentLookUpError = function isAcceptableDocumentLookUpError(err) {
        return this.expectedAggregateInterruptErrors.includes(err.code);
    };

    /**
     * Returns a copy for the given analyzeShardKey response with the "numReadsByRange" and
     * "numWritesByRange" fields truncated if they exist since they are arrays of length
     * this.numRanges (defaults to 100).
     */
    $config.data.truncateAnalyzeShardKeyResponseForLogging =
        function truncateAnalyzeShardKeyResponseForLogging(originalRes) {
        const truncatedRes = Object.extend({}, originalRes, true /* deep */);
        if (truncatedRes.hasOwnProperty("readDistribution") &&
            truncatedRes.readDistribution.sampleSize.total > 0) {
            truncatedRes.readDistribution["numReadsByRange"] = "truncated";
        }
        if (truncatedRes.hasOwnProperty("writeDistribution") &&
            truncatedRes.writeDistribution.sampleSize.total > 0) {
            truncatedRes.writeDistribution["numWritesByRange"] = "truncated";
        }
        return truncatedRes;
    };

    ////
    // The body of the workload.

    $config.setup = function setup(db, collName, cluster) {
        // Look up the number of most common values and the number of ranges that the
        // analyzeShardKey command should return.
        cluster.executeOnMongodNodes((db) => {
            const res = assert.commandWorked(db.adminCommand({
                getParameter: 1,
                analyzeShardKeyNumMostCommonValues: 1,
                analyzeShardKeyNumRanges: 1
            }));
            if (this.analyzeShardKeyNumMostCommonValues === undefined) {
                this.analyzeShardKeyNumMostCommonValues = res.analyzeShardKeyNumMostCommonValues;
            } else {
                assert.eq(this.analyzeShardKeyNumMostCommonValues,
                          res.analyzeShardKeyNumMostCommonValues);
            }
            if (this.analyzeShardKeyNumRanges === undefined) {
                this.analyzeShardKeyNumRanges = res.analyzeShardKeyNumRanges;
            } else {
                assert.eq(this.analyzeShardKeyNumRanges, res.analyzeShardKeyNumRanges);
            }
        });

        // Force all mongoses and mongods to only sample queries that are explicitly marked
        // as eligible for sampling.
        if (cluster.isSharded) {
            cluster.executeOnMongosNodes((adminDb) => {
                configureFailPoint(adminDb,
                                   "queryAnalysisSamplerFilterByComment",
                                   {comment: this.eligibleForSamplingComment});
            });
        }
        cluster.executeOnMongodNodes((adminDb) => {
            configureFailPoint(adminDb,
                               "queryAnalysisSamplerFilterByComment",
                               {comment: this.eligibleForSamplingComment});
        });

        // On a sharded cluster, running an aggregate command by default involves running getMore
        // commands since the cursor establisher in sharding is pessimistic about the router being
        // stale so it always makes a cursor with {batchSize: 0} on the shards and then run getMore
        // commands afterwards because this helps avoid doing any storage work and instead only pins
        // the filtering metadata which would be used for the cursor. Interrupts such as
        // stepdowns can cause a getMore command get fail as a result of the cursor being killed.
        this.expectedAggregateInterruptErrors =
            cluster.isSharded() && TestData.runningWithShardStepdowns ? aggregateInterruptErrors
                                                                      : [];

        this.generateShardKeyOptions(cluster);
        this.generateDocumentOptions(cluster);
        this.generateInitialDocuments(db, collName, cluster);
        this.generateRandomQueryPatterns();

        // The unique id of the document storing the latest metrics returned by the analyzeShardKey
        // command.
        this.metricsDocIdString = extractUUIDFromObject(UUID());
    };

    $config.teardown = function teardown(db, collName, cluster) {
        if (cluster.isSharded) {
            cluster.executeOnMongosNodes((adminDb) => {
                configureFailPoint(adminDb, "queryAnalysisSamplerFilterByComment", {}, "off");
            });
        }
        cluster.executeOnMongodNodes((adminDb) => {
            configureFailPoint(adminDb, "queryAnalysisSamplerFilterByComment", {}, "off");
        });

        const res = assert.commandWorked(db.runCommand(
            {find: this.metricsCollName, filter: {_id: new UUID(this.metricsDocIdString)}}));
        assert.eq(res.cursor.id, 0, res);
        assert.eq(res.cursor.firstBatch.length, 1, res);
        const metrics = res.cursor.firstBatch[0].metrics;
        print("Doing final validation of read and write distribution metrics " +
              tojson(this.truncateAnalyzeShardKeyResponseForLogging(metrics)));
        this.assertReadWriteDistributionMetrics(metrics, true /* isFinal */);
    };

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, [db, collName]);
        this.metricsDocId = new UUID(this.metricsDocIdString);
    };

    $config.states.analyzeShardKey = function analyzeShardKey(db, collName) {
        print("Starting analyzeShardKey state");
        const ns = db.getName() + "." + collName;
        const res = db.adminCommand({analyzeShardKey: ns, key: this.shardKeyOptions.shardKey});
        try {
            assert.commandWorked(res);
            print("Metrics: " +
                  tojsononeline({res: this.truncateAnalyzeShardKeyResponseForLogging(res)}));
            this.assertKeyCharacteristicsMetrics(res);
            this.assertReadWriteDistributionMetrics(res, false /* isFinal */);
            // Persist the metrics so we can do the final validation during teardown.
            assert.commandWorked(
                db[this.metricsCollName].update({_id: this.metricsDocId},
                                                {_id: this.metricsDocId, collName, metrics: res},
                                                true /* upsert */));
        } catch (e) {
            if (!this.isAcceptableAnalyzeShardKeyError(res)) {
                throw e;
            }
        }
        print("Finished analyzeShardKey state");
    };

    $config.states.enableQuerySampling = function enableQuerySampling(db, collName) {
        print("Starting enableQuerySampling state");
        const ns = db.getName() + "." + collName;
        assert.commandWorked(db.adminCommand({
            configureQueryAnalyzer: ns,
            mode: "full",
            sampleRate: AnalyzeShardKeyUtil.getRandInteger(this.minSampleRate, this.maxSampleRate)
        }));
        print("Finished enableQuerySampling state");
    };

    $config.states.disableQuerySampling = function disableQuerySampling(db, collName) {
        print("Starting disableQuerySampling state");
        const ns = db.getName() + "." + collName;
        // If query sampling is off, this command is expected to fail with an IllegalOperation
        // error.
        assert.commandWorkedOrFailedWithCode(
            db.adminCommand({configureQueryAnalyzer: ns, mode: "off"}),
            ErrorCodes.IllegalOperation);
        print("Finished disableQuerySampling state");
    };

    $config.states.find = function find(db, collName) {
        const cmdObj = {
            find: collName,
            filter: this.generateRandomReadFilter(),
            singleBatch: true,  // Avoid leaving open cursors.
            comment: this.eligibleForSamplingComment
        };
        print("Starting find state " + tojsononeline(cmdObj));
        const res = assert.commandWorked(db.runCommand(cmdObj));
        assert.eq(res.cursor.id, 0, res);
        print("Finished find state");
    };

    $config.states.aggregate = function aggregate(db, collName) {
        const cmdObj = {
            aggregate: collName,
            pipeline: [
                {$match: this.generateRandomReadFilter()},
                {$limit: 10}  // Avoid leaving open cursors.
            ],
            cursor: {},
            comment: this.eligibleForSamplingComment
        };
        print("Starting aggregate state " + tojsononeline(cmdObj));
        const res = assert.commandWorkedOrFailedWithCode(db.runCommand(cmdObj),
                                                         this.expectedAggregateInterruptErrors);
        if (res.ok) {
            assert.eq(res.cursor.id, 0, res);
        }
        print("Finished aggregate state");
    };

    $config.states.count = function count(db, collName) {
        const cmdObj = {
            count: collName,
            query: this.generateRandomReadFilter(),
            comment: this.eligibleForSamplingComment
        };
        print("Starting count state " + tojsononeline(cmdObj));
        assert.commandWorked(db.runCommand(cmdObj));
        print("Finished count state");
    };

    $config.states.distinct = function distinct(db, collName) {
        const cmdObj = {
            distinct: collName,
            key: this.candidateShardKeyFieldName,
            query: this.generateRandomReadFilter(),
            comment: this.eligibleForSamplingComment
        };
        print("Starting distinct state " + tojsononeline(cmdObj));
        assert.commandWorked(db.runCommand(cmdObj));
        print("Finished distinct state");
    };

    $config.states.update = function update(db, collName) {
        const filter = this.tryGenerateRandomWriteFilter(db, collName);
        if (!filter) {
            return;
        }
        const update = this.generateRandomModifierUpdate();
        const isMulti = Math.random() < this.probabilityOfMultiWrites;

        const cmdObj = {
            update: collName,
            updates: [{q: filter, u: update, multi: isMulti}],
            comment: this.eligibleForSamplingComment
        };
        print("Starting update state " + tojsononeline(cmdObj));
        const res = db.runCommand(cmdObj);
        try {
            assert.commandWorked(res);
            assert.eq(res.nModified, 1, {cmdObj, res});
            assert.eq(res.n, 1, {cmdObj, res});
        } catch (e) {
            if (!this.isAcceptableUpdateError(res) &&
                !(res.hasOwnProperty("writeErrors") &&
                  isAcceptableUpdateError(res.writeErrors[0]))) {
                throw e;
            }
        }
        print("Finished update state");
    };

    $config.states.remove = function remove(db, collName) {
        const filter = this.tryGenerateRandomWriteFilter(db, collName);
        if (!filter) {
            return;
        }
        const isMulti = Math.random() < this.probabilityOfMultiWrites;

        const cmdObj = {
            delete: collName,
            deletes: [{q: filter, limit: isMulti ? 0 : 1}],
            comment: this.eligibleForSamplingComment
        };
        print("Starting remove state " + tojsononeline(cmdObj));
        const res = assert.commandWorked(db.runCommand(cmdObj));
        assert.eq(res.n, 1, {cmdObj, res});
        // Insert a random document to restore the original number of documents.
        assert.commandWorked(
            db.runCommand({insert: collName, documents: [this.generateRandomDocument(this.tid)]}));
        print("Finished remove state");
    };

    $config.states.findAndModifyUpdate = function findAndModifyUpdate(db, collName) {
        const filter = this.tryGenerateRandomWriteFilter(db, collName);
        if (!filter) {
            return;
        }
        const update = this.generateRandomModifierUpdate();

        const cmdObj = {
            findAndModify: collName,
            query: filter,
            update,
            comment: this.eligibleForSamplingComment
        };
        print("Starting findAndModifyUpdate state " + tojsononeline(cmdObj));
        const res = db.runCommand(cmdObj);
        try {
            assert.commandWorked(res);
            assert.eq(res.lastErrorObject.n, 1, {cmdObj, res});
            assert.eq(res.lastErrorObject.updatedExisting, true, {cmdObj, res});
        } catch (e) {
            if (!this.isAcceptableUpdateError(res)) {
                throw e;
            }
        }
        print("Finished findAndModifyUpdate state");
    };

    $config.states.findAndModifyRemove = function findAndModifyRemove(db, collName) {
        const filter = this.tryGenerateRandomWriteFilter(db, collName);
        if (!filter) {
            return;
        }

        const cmdObj = {
            findAndModify: collName,
            query: filter,
            remove: true,
            comment: this.eligibleForSamplingComment
        };
        print("Starting findAndModifyRemove state " + tojsononeline(cmdObj));
        const res = assert.commandWorked(db.runCommand(cmdObj));
        assert.eq(res.lastErrorObject.n, 1, {cmdObj, res});
        // Insert a random document to restore the original number of documents.
        assert.commandWorked(
            db.runCommand({insert: collName, documents: [this.generateRandomDocument(this.tid)]}));
        print("Finished findAndModifyRemove state");
    };

    if ($config.passConnectionCache) {
        // If 'passConnectionCache' is true, every state function must accept 3 parameters: db,
        // collName and connCache. This workload does not set 'passConnectionCache' since it doesn't
        // use 'connCache' but it may extend a sharding workload that uses it.
        const originalInit = $config.states.init;
        $config.states.init = function(db, collName, connCache) {
            originalInit.call(this, db, collName);
        };

        const originalEnableQuerySampling = $config.states.enableQuerySampling;
        $config.states.enableQuerySampling = function(db, collName, connCache) {
            originalEnableQuerySampling.call(this, db, collName);
        };

        const originalDisableQuerySampling = $config.states.disableQuerySampling;
        $config.states.disableQuerySampling = function(db, collName, connCache) {
            originalDisableQuerySampling.call(this, db, collName);
        };

        const originalAnalyzeShardKey = $config.states.analyzeShardKey;
        $config.states.analyzeShardKey = function(db, collName, connCache) {
            originalAnalyzeShardKey.call(this, db, collName);
        };

        const originalFind = $config.states.find;
        $config.states.find = function(db, collName, connCache) {
            originalFind.call(this, db, collName);
        };

        const originalAggregate = $config.states.aggregate;
        $config.states.aggregate = function(db, collName, connCache) {
            originalAggregate.call(this, db, collName);
        };

        const originalCount = $config.states.count;
        $config.states.count = function(db, collName, connCache) {
            originalCount.call(this, db, collName);
        };

        const originalDistinct = $config.states.distinct;
        $config.states.distinct = function(db, collName, connCache) {
            originalDistinct.call(this, db, collName);
        };

        const originalInsert = $config.states.insert;
        $config.states.insert = function(db, collName, connCache) {
            originalInsert.call(this, db, collName);
        };

        const originalUpdate = $config.states.update;
        $config.states.update = function(db, collName, connCache) {
            originalUpdate.call(this, db, collName);
        };

        const originalRemove = $config.states.remove;
        $config.states.remove = function(db, collName, connCache) {
            originalRemove.call(this, db, collName);
        };

        const originalFindAndModifyUpdate = $config.states.findAndModifyUpdate;
        $config.states.findAndModifyUpdate = function(db, collName, connCache) {
            originalFindAndModifyUpdate.call(this, db, collName);
        };

        const originalFindAndModifyRemove = $config.states.findAndModifyRemove;
        $config.states.findAndModifyRemove = function(db, collName, connCache) {
            originalFindAndModifyRemove.call(this, db, collName);
        };
    }

    $config.transitions = {
        init: {
            enableQuerySampling: 1,
        },
        analyzeShardKey: {
            enableQuerySampling: 0.18,
            disableQuerySampling: 0.02,
            find: 0.1,
            aggregate: 0.1,
            count: 0.1,
            distinct: 0.1,
            update: 0.1,
            remove: 0.1,
            findAndModifyUpdate: 0.1,
            findAndModifyRemove: 0.1,
        },
        enableQuerySampling: {
            analyzeShardKey: 0.18,
            disableQuerySampling: 0.02,
            find: 0.1,
            aggregate: 0.1,
            count: 0.1,
            distinct: 0.1,
            update: 0.1,
            remove: 0.1,
            findAndModifyUpdate: 0.1,
            findAndModifyRemove: 0.1,
        },
        disableQuerySampling: {
            analyzeShardKey: 0.05,
            enableQuerySampling: 0.95,
        },
        find: {
            analyzeShardKey: 0.2,
            enableQuerySampling: 0.1,
            aggregate: 0.1,
            count: 0.1,
            distinct: 0.1,
            update: 0.1,
            remove: 0.1,
            findAndModifyUpdate: 0.1,
            findAndModifyRemove: 0.1,
        },
        aggregate: {
            analyzeShardKey: 0.2,
            enableQuerySampling: 0.1,
            find: 0.1,
            count: 0.1,
            distinct: 0.1,
            update: 0.1,
            remove: 0.1,
            findAndModifyUpdate: 0.1,
            findAndModifyRemove: 0.1,
        },
        count: {
            analyzeShardKey: 0.2,
            enableQuerySampling: 0.1,
            find: 0.1,
            aggregate: 0.1,
            distinct: 0.1,
            update: 0.1,
            remove: 0.1,
            findAndModifyUpdate: 0.1,
            findAndModifyRemove: 0.1,
        },
        distinct: {
            analyzeShardKey: 0.2,
            enableQuerySampling: 0.1,
            find: 0.1,
            aggregate: 0.1,
            count: 0.1,
            update: 0.1,
            remove: 0.1,
            findAndModifyUpdate: 0.1,
            findAndModifyRemove: 0.1,
        },
        update: {
            analyzeShardKey: 0.2,
            enableQuerySampling: 0.1,
            find: 0.1,
            aggregate: 0.1,
            count: 0.1,
            distinct: 0.1,
            remove: 0.1,
            findAndModifyUpdate: 0.1,
            findAndModifyRemove: 0.1,
        },
        remove: {
            analyzeShardKey: 0.2,
            enableQuerySampling: 0.1,
            find: 0.1,
            aggregate: 0.1,
            count: 0.1,
            distinct: 0.1,
            update: 0.1,
            findAndModifyUpdate: 0.1,
            findAndModifyRemove: 0.1,
        },
        findAndModifyUpdate: {
            analyzeShardKey: 0.2,
            enableQuerySampling: 0.1,
            find: 0.1,
            aggregate: 0.1,
            count: 0.1,
            distinct: 0.1,
            update: 0.1,
            remove: 0.1,
            findAndModifyRemove: 0.1,
        },
        findAndModifyRemove: {
            analyzeShardKey: 0.2,
            enableQuerySampling: 0.1,
            find: 0.1,
            aggregate: 0.1,
            count: 0.1,
            distinct: 0.1,
            update: 0.1,
            remove: 0.1,
            findAndModifyUpdate: 0.1,
        }
    };

    return $config;
});
