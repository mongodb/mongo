var defragmentationUtil = (function() {
    load("jstests/libs/feature_flag_util.js");
    load("jstests/sharding/libs/find_chunks_util.js");

    let createFragmentedCollection = function(mongos,
                                              ns,
                                              numChunks,
                                              maxChunkFillMB,
                                              numZones,
                                              docSizeBytesRange,
                                              chunkSpacing,
                                              disableCollectionBalancing) {
        jsTest.log("Creating fragmented collection " + ns +
                   " with parameters: numChunks = " + numChunks + ", numZones = " + numZones +
                   ", docSizeBytesRange = " + docSizeBytesRange +
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
        // Created zones will line up exactly with existing chunks so as not to trigger zone
        // violations in the balancer.
        createRandomZones(mongos, ns, numZones);
        fillChunksToRandomSize(mongos, ns, docSizeBytesRange, maxChunkFillMB);

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

    let createRandomZones = function(mongos, ns, numZones) {
        let existingChunks = findChunksUtil.findChunksByNs(mongos.getDB('config'), ns);
        existingChunks = Array.shuffle(existingChunks.toArray());
        for (let i = 0; i < numZones; i++) {
            let zoneName = "Zone" + i;
            let shardForZone = existingChunks[i].shard;
            assert.commandWorked(
                mongos.adminCommand({addShardToZone: shardForZone, zone: zoneName}));
            assert.commandWorked(mongos.adminCommand({
                updateZoneKeyRange: ns,
                min: existingChunks[i].min,
                max: existingChunks[i].max,
                zone: zoneName
            }));
        }
    };

    let fillChunksToRandomSize = function(mongos, ns, docSizeBytesRange, maxChunkFillMB) {
        const chunks = findChunksUtil.findChunksByNs(mongos.getDB('config'), ns).toArray();
        const coll = mongos.getCollection(ns);
        let bulk = coll.initializeUnorderedBulkOp();
        assert.gte(docSizeBytesRange[1], docSizeBytesRange[0]);
        chunks.forEach((chunk) => {
            let chunkSize = Random.randInt(maxChunkFillMB);
            let docSizeBytes = Random.randInt(docSizeBytesRange[1] - docSizeBytesRange[0] + 1) +
                docSizeBytesRange[0];
            docSizeBytes = docSizeBytes === 0 ? 1 : docSizeBytes;
            let bigString = "X".repeat(docSizeBytes);
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

    let checkPostDefragmentationState = function(configSvr, mongos, ns, maxChunkSizeMB, shardKey) {
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
                    assert(false,
                           `Chunks ${tojson(leftChunk)} and ${
                               tojson(rightChunk)} should have been merged`);
                }
            }
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
