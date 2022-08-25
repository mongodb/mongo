var defragmentationUtil = (function() {
    load("jstests/libs/feature_flag_util.js");
    load("jstests/sharding/libs/find_chunks_util.js");

    let createFragmentedCollection = function(mongos,
                                              ns,
                                              numChunks,
                                              maxChunkFillMB,
                                              numZones,
                                              docSizeBytes,
                                              chunkSpacing,
                                              disableCollectionBalancing) {
        jsTest.log("Creating fragmented collection " + ns + " with parameters: numChunks = " +
                   numChunks + ", numZones = " + numZones + ", docSizeBytes = " + docSizeBytes +
                   ", maxChunkFillMB = " + maxChunkFillMB + ", chunkSpacing = " + chunkSpacing);
        assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {key: 1}}));
        // Turn off balancer for this collection
        if (disableCollectionBalancing) {
            // Use 'retryWrites' when writing to the configsvr because mongos does not automatically
            // retry those.
            const mongosSession = mongos.startSession({retryWrites: true});
            const sessionConfigDB = mongosSession.getDatabase('config');
            assert.commandWorked(
                sessionConfigDB.collections.update({_id: ns}, {$set: {"noBalance": true}}));
        }

        createAndDistributeChunks(mongos, ns, numChunks, chunkSpacing);
        createRandomZones(mongos, ns, numZones, chunkSpacing);
        fillChunksToRandomSize(mongos, ns, docSizeBytes, maxChunkFillMB);

        const beginningNumberChunks = findChunksUtil.countChunksForNs(mongos.getDB('config'), ns);
        const beginningNumberZones = mongos.getDB('config').tags.countDocuments({ns: ns});
        jsTest.log("Collection " + ns + " created with " + beginningNumberChunks + " chunks and " +
                   beginningNumberZones + " zones.");
    };

    let createAndDistributeChunks = function(mongos, ns, numChunks, chunkSpacing) {
        const shards = mongos.getCollection('config.shards').find().toArray();
        const existingNumChunks = findChunksUtil.countChunksForNs(mongos.getDB('config'), ns);
        let numChunksToCreate = numChunks - existingNumChunks;
        if (numChunksToCreate <= 0) {
            return;
        }
        for (let i = -Math.floor(numChunksToCreate / 2); i < Math.ceil(numChunksToCreate / 2);
             i++) {
            assert.commandWorked(mongos.adminCommand({split: ns, middle: {key: i * chunkSpacing}}));
            assert.soon(() => {
                let toShard = Random.randInt(shards.length);
                let res = mongos.adminCommand(
                    {moveChunk: ns, find: {key: i * chunkSpacing}, to: shards[toShard]._id});
                return res.ok;
            });
        }
    };

    let createRandomZones = function(mongos, ns, numZones, chunkSpacing) {
        for (let i = -Math.floor(numZones / 2); i < Math.ceil(numZones / 2); i++) {
            let zoneName = "Zone" + i;
            let shardForZone =
                findChunksUtil
                    .findOneChunkByNs(mongos.getDB('config'), ns, {min: {key: i * chunkSpacing}})
                    .shard;
            assert.commandWorked(
                mongos.adminCommand({addShardToZone: shardForZone, zone: zoneName}));
            assert.commandWorked(mongos.adminCommand({
                updateZoneKeyRange: ns,
                min: {key: i * chunkSpacing},
                max: {key: i * chunkSpacing + chunkSpacing},
                zone: zoneName
            }));
        }
    };

    let fillChunksToRandomSize = function(mongos, ns, docSizeBytes, maxChunkFillMB) {
        const chunks = findChunksUtil.findChunksByNs(mongos.getDB('config'), ns).toArray();
        const bigString = "X".repeat(docSizeBytes);
        const coll = mongos.getCollection(ns);
        let bulk = coll.initializeUnorderedBulkOp();
        chunks.forEach((chunk) => {
            let chunkSize = Random.randInt(maxChunkFillMB);
            let docsPerChunk = (chunkSize * 1024 * 1024) / docSizeBytes;
            if (docsPerChunk === 0) {
                return;
            }
            let minKey = chunk["min"]["key"];
            if (minKey === MinKey)
                minKey = Number.MIN_SAFE_INTEGER;
            let maxKey = chunk["max"]["key"];
            if (maxKey === MaxKey)
                maxKey = Number.MAX_SAFE_INTEGER;
            let currKey = minKey;
            const gap = ((maxKey - minKey) / docsPerChunk);
            for (let i = 0; i < docsPerChunk; i++) {
                bulk.insert({key: currKey, longString: bigString});
                currKey += gap;
            }
        });
        assert.commandWorked(bulk.execute());
    };

    let checkForOversizedChunk = function(
        coll, chunk, shardKey, avgObjSize, oversizedChunkThreshold) {
        let chunkSize = coll.countDocuments(
                            {[shardKey]: {$gte: chunk.min[shardKey], $lt: chunk.max[shardKey]}}) *
            avgObjSize;
        assert.lte(
            chunkSize,
            oversizedChunkThreshold,
            `Chunk ${tojson(chunk)} has size ${chunkSize} which is greater than max chunk size of ${
                oversizedChunkThreshold}`);
    };

    let checkForMergeableChunkSiblings = function(
        coll, leftChunk, rightChunk, shardKey, avgObjSize, oversizedChunkThreshold) {
        let combinedDataSize =
            coll.countDocuments(
                {[shardKey]: {$gte: leftChunk.min[shardKey], $lt: rightChunk.max[shardKey]}}) *
            avgObjSize;
        // The autosplitter should not split chunks whose combined size is < 133% of
        // maxChunkSize but this threshold may be off by a few documents depending on
        // rounding of avgObjSize.
        const autosplitRoundingTolerance = 3 * avgObjSize;
        assert.gte(combinedDataSize,
                   oversizedChunkThreshold - autosplitRoundingTolerance,
                   `Chunks ${tojson(leftChunk)} and ${
                       tojson(rightChunk)} are mergeable with combined size ${combinedDataSize}`);
    };

    let checkPostDefragmentationState = function(configSvr, mongos, ns, maxChunkSizeMB, shardKey) {
        const withAutoSplitActive =
            !FeatureFlagUtil.isEnabled(configSvr.getDB('admin'), 'NoMoreAutoSplitter');
        jsTest.log(`Chunk (auto)splitting functionalities assumed to be ${
            withAutoSplitActive ? "ON" : "OFF"}`);
        const oversizedChunkThreshold = maxChunkSizeMB * 1024 * 1024 * 4 / 3;
        const chunks = findChunksUtil.findChunksByNs(mongos.getDB('config'), ns)
                           .sort({[shardKey]: 1})
                           .toArray();
        const coll = mongos.getCollection(ns);
        const pipeline = [
            {'$collStats': {'storageStats': {}}},
            {'$project': {'shard': true, 'storageStats': {'avgObjSize': true}}}
        ];
        const storageStats = coll.aggregate(pipeline).toArray();
        let avgObjSizeByShard = {};
        storageStats.forEach((storageStat) => {
            avgObjSizeByShard[storageStat['shard']] =
                typeof (storageStat['storageStats']['avgObjSize']) === "undefined"
                ? 0
                : storageStat['storageStats']['avgObjSize'];
        });
        for (let i = 1; i < chunks.length; i++) {
            let leftChunk = chunks[i - 1];
            let rightChunk = chunks[i];
            // Check for mergeable chunks with combined size less than maxChunkSize
            if (leftChunk["shard"] === rightChunk["shard"] &&
                bsonWoCompare(leftChunk["max"], rightChunk["min"]) === 0) {
                let leftChunkZone = getZoneForRange(mongos, ns, leftChunk.min, leftChunk.max);
                let rightChunkZone = getZoneForRange(mongos, ns, rightChunk.min, rightChunk.max);
                if (bsonWoCompare(leftChunkZone, rightChunkZone) === 0) {
                    if (withAutoSplitActive) {
                        checkForMergeableChunkSiblings(coll,
                                                       leftChunk,
                                                       rightChunk,
                                                       shardKey,
                                                       avgObjSizeByShard[leftChunk['shard']],
                                                       oversizedChunkThreshold);
                    } else {
                        assert(false,
                               `Chunks ${tojson(leftChunk)} and ${
                                   tojson(rightChunk)} should have been merged`);
                    }
                }
            }
            if (withAutoSplitActive) {
                checkForOversizedChunk(coll,
                                       leftChunk,
                                       shardKey,
                                       avgObjSizeByShard[leftChunk['shard']],
                                       oversizedChunkThreshold);
            }
        }

        if (withAutoSplitActive) {
            const lastChunk = chunks[chunks.length - 1];
            checkForOversizedChunk(coll,
                                   lastChunk,
                                   shardKey,
                                   avgObjSizeByShard[lastChunk['shard']],
                                   oversizedChunkThreshold);
        }
    };

    let getZoneForRange = function(mongos, ns, minKey, maxKey) {
        const tags = mongos.getDB('config')
                         .tags.find({ns: ns, min: {$lte: minKey}, max: {$gte: maxKey}})
                         .toArray();
        assert.lte(tags.length, 1);
        if (tags.length === 1) {
            return tags[0].tag;
        }
        return null;
    };

    let waitForEndOfDefragmentation = function(mongos, ns) {
        jsTest.log("Waiting end of defragmentation for " + ns);
        assert.soon(function() {
            let balancerStatus =
                assert.commandWorked(mongos.adminCommand({balancerCollectionStatus: ns}));

            if (balancerStatus.balancerCompliant) {
                // As we can't rely on `balancerCompliant` due to orphan counter non atomic update,
                // we need to ensure the collection is balanced by some extra checks
                sh.awaitCollectionBalance(mongos.getCollection(ns));
            }

            return balancerStatus.balancerCompliant ||
                balancerStatus.firstComplianceViolation !== 'defragmentingChunks';
        });
        jsTest.log("Defragmentation completed for " + ns);
    };

    return {
        createFragmentedCollection,
        checkPostDefragmentationState,
        getZoneForRange,
        waitForEndOfDefragmentation,
    };
})();
