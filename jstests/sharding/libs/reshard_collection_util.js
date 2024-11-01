/**
 * Util class for testing reshardCollection cmd.
 */

import {extractUUIDFromObject, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {ShardedIndexUtil} from "jstests/sharding/libs/sharded_index_util.js";

export class ReshardCollectionCmdTest {
    constructor(testConfig) {
        assert((testConfig.mongos || testConfig.st) && testConfig.dbName && testConfig.collName &&
               testConfig.numInitialDocs !== undefined);

        // Direct shard checks are only possible with a ShardingTest object.
        if (!testConfig.st) {
            assert(testConfig.skipDirectShardChecks);
        }
        this._st = testConfig.st;
        this._mongos = testConfig.mongos ? testConfig.mongos : this._st.s0;
        this._mongosConfig = this._mongos.getDB('config');
        this._dbName = testConfig.dbName;
        this._collName = testConfig.collName;
        this._ns = this._dbName + "." + this._collName;
        this._numInitialDocs = testConfig.numInitialDocs;
        this._skipDirectShardChecks = testConfig.skipDirectShardChecks != undefined
            ? testConfig.skipDirectShardChecks
            : false;
        this._skipCollectionSetup =
            testConfig.skipCollectionSetup ? testConfig.skipCollectionSetup : false;
        this._timeseries = testConfig.timeseries;

        this._shardToRSMap = {};
        this._shardIdToShardMap = {};

        if (!this._skipDirectShardChecks) {
            // Build the mapping for all shards assuming the shard number is continuously
            // increasing.
            let i = 0;
            while (this._st["shard" + i]) {
                this._shardToRSMap[this._st["shard" + i].shardName] = this._st["rs" + i];
                this._shardIdToShardMap[this._st["shard" + i].shardName] = this._st["shard" + i];
                i++;
            }
        }
    }

    _getUUIDFromCollectionInfo(dbName, collName) {
        const uuidObject = getUUIDFromListCollections(
            this._mongos.getDB(dbName), this._timeseries ? `system.buckets.${collName}` : collName);

        return extractUUIDFromObject(uuidObject);
    }

    _constructTemporaryReshardingCollName(dbName, collName) {
        const existingUUID = this._getUUIDFromCollectionInfo(dbName, collName);
        return 'system.resharding.' + existingUUID;
    }

    _getAllShardIdsFromExpectedChunks(expectedChunks) {
        let shardIds = new Set();
        expectedChunks.forEach(chunk => {
            shardIds.add(chunk.recipientShardId);
        });
        return shardIds;
    }

    _verifyChunksMatchExpected(numExpectedChunks, presetExpectedChunks) {
        let collEntry = this._mongos.getDB('config').getCollection('collections').findOne({
            _id: this._timeseries ? `${this._dbName}.system.buckets.${this._collName}` : this._ns
        });
        let chunkQuery = {uuid: collEntry.uuid};

        const reshardedChunks = this._mongosConfig.chunks.find(chunkQuery).toArray();

        if (presetExpectedChunks) {
            presetExpectedChunks.sort();
        }

        reshardedChunks.sort();
        assert.eq(numExpectedChunks, reshardedChunks.length, tojson(reshardedChunks));

        let shardChunkCounts = {};
        let incChunkCount = key => {
            if (shardChunkCounts.hasOwnProperty(key)) {
                shardChunkCounts[key]++;
            } else {
                shardChunkCounts[key] = 1;
            }
        };

        for (let i = 0; i < numExpectedChunks; i++) {
            incChunkCount(reshardedChunks[i].shard);

            // match exact chunk boundaries for presetExpectedChunks
            if (presetExpectedChunks) {
                assert.eq(presetExpectedChunks[i].recipientShardId, reshardedChunks[i].shard);
                assert.eq(presetExpectedChunks[i].min, reshardedChunks[i].min);
                assert.eq(presetExpectedChunks[i].max, reshardedChunks[i].max);
            }
        }

        // if presetChunks not specified, we only assert that chunks counts are balanced across
        // shards
        if (!presetExpectedChunks) {
            let maxDiff = 0;
            let shards = Object.keys(shardChunkCounts);

            shards.forEach(shard1 => {
                shards.forEach(shard2 => {
                    let diff = Math.abs(shardChunkCounts[shard1] - shardChunkCounts[shard2]);
                    maxDiff = (diff > maxDiff) ? diff : maxDiff;
                });
            });

            assert.lte(maxDiff, 1, tojson(reshardedChunks));
        }
    }

    _verifyCollectionExistenceForConn(collName, expectedToExist, conn) {
        const doesExist = Boolean(conn.getDB(this._dbName)[collName].exists());
        assert.eq(doesExist, expectedToExist);
    }

    _verifyTemporaryReshardingCollectionExistsWithCorrectOptions(shardKey,
                                                                 expectedRecipientShards) {
        const tempReshardingCollName =
            this._constructTemporaryReshardingCollName(this._dbName, this._collName);
        this._verifyCollectionExistenceForConn(tempReshardingCollName, false, this._mongos);

        if (!this._skipDirectShardChecks) {
            expectedRecipientShards.forEach(shardId => {
                const rsPrimary = this._shardToRSMap[shardId].getPrimary();
                this._verifyCollectionExistenceForConn(this._collName, true, rsPrimary);
                this._verifyCollectionExistenceForConn(tempReshardingCollName, false, rsPrimary);
                ShardedIndexUtil.assertIndexExistsOnShard(
                    this._shardIdToShardMap[shardId], this._dbName, this._collName, shardKey);
            });
        }
    }

    _verifyAllShardingCollectionsRemoved(tempReshardingCollName) {
        assert.eq(0, this._mongos.getDB(this._dbName)[tempReshardingCollName].find().itcount());
        assert.eq(0, this._mongosConfig.reshardingOperations.find({ns: this._ns}).itcount());
        assert.eq(
            0,
            this._mongosConfig.collections.find({_id: this._ns, reshardingFields: {$exists: true}})
                .itcount());

        if (!this._skipDirectShardChecks) {
            assert.eq(0,
                      this._st.rs0.getPrimary()
                          .getDB('config')
                          .localReshardingOperations.donor.find({ns: this._ns})
                          .itcount());
            assert.eq(0,
                      this._st.rs0.getPrimary()
                          .getDB('config')
                          .localReshardingOperations.recipient.find({ns: this._ns})
                          .itcount());
            assert.eq(0,
                      this._st.rs1.getPrimary()
                          .getDB('config')
                          .localReshardingOperations.donor.find({ns: this._ns})
                          .itcount());
            assert.eq(0,
                      this._st.rs1.getPrimary()
                          .getDB('config')
                          .localReshardingOperations.recipient.find({ns: this._ns})
                          .itcount());
        }
    }

    _verifyTagsDocumentsAfterOperationCompletes(ns, shardKeyPattern, expectedZones) {
        const tagsArr = this._mongos.getCollection('config.tags').find({ns: ns}).toArray();
        if (expectedZones !== undefined) {
            assert.eq(tagsArr.length, expectedZones.length);
            tagsArr.sort((a, b) => a["tag"].localeCompare(b["tag"]));
            expectedZones.sort((a, b) => a["zone"].localeCompare(b["zone"]));
        }
        for (let i = 0; i < tagsArr.length; ++i) {
            assert.eq(Object.keys(tagsArr[i]["min"]), shardKeyPattern);
            assert.eq(Object.keys(tagsArr[i]["max"]), shardKeyPattern);
            if (expectedZones !== undefined) {
                assert.eq(tagsArr[i]["min"], expectedZones[i]["min"]);
                assert.eq(tagsArr[i]["max"], expectedZones[i]["max"]);
                assert.eq(tagsArr[i]["tag"], expectedZones[i]["zone"]);
            }
        }
    }

    _verifyIndexesCreated(oldIndexes, shardKey) {
        const indexes = this._mongos.getDB(this._dbName).getCollection(this._collName).getIndexes();
        const indexKeySet = new Set();
        indexes.forEach(index => indexKeySet.add(tojson(index.key)));
        assert.eq(indexKeySet.has(tojson(shardKey)), true);
        oldIndexes.forEach(index => {
            assert.eq(indexKeySet.has(tojson(index.key)), true);
        });
    }

    _verifyShardKey(expectedShardKey) {
        const collName = this._timeseries ? "system.buckets." + this._collName : this._collName;
        const shardKey = this._mongos.getDB(this._dbName)[collName].getShardKey();
        assert.eq(shardKey, expectedShardKey);
    }

    _verifyDocumentsExist(docs, collection) {
        docs.forEach(doc => {
            assert.eq(collection.find(doc).count(), 1, `Missing document ${tojson(doc)}`);
        });
    }

    assertReshardCollOkWithPreset(commandObj, presetReshardedChunks) {
        if (!this._skipCollectionSetup) {
            const oldShardKey = {oldKey: 1};
            assert.commandWorked(
                this._mongos.adminCommand({shardCollection: this._ns, key: oldShardKey}));
            this._verifyShardKey(oldShardKey);
        }

        let docs = [];
        let collection = this._mongos.getDB(this._dbName).getCollection(this._collName);
        let bulk = collection.initializeOrderedBulkOp();
        for (let x = 0; x < this._numInitialDocs; x++) {
            let doc = {oldKey: x, newKey: this._numInitialDocs - x};
            docs.push(doc);
            bulk.insert(doc);
        }
        assert.commandWorked(bulk.execute());

        commandObj._presetReshardedChunks = presetReshardedChunks;
        const tempReshardingCollName =
            this._constructTemporaryReshardingCollName(this._dbName, this._collName);

        assert.commandWorked(this._mongos.adminCommand(commandObj));

        this._verifyShardKey(commandObj.key);

        this._verifyTemporaryReshardingCollectionExistsWithCorrectOptions(
            commandObj.key, this._getAllShardIdsFromExpectedChunks(presetReshardedChunks));

        this._verifyTagsDocumentsAfterOperationCompletes(this._ns, Object.keys(commandObj.key));

        this._verifyChunksMatchExpected(presetReshardedChunks.length, presetReshardedChunks);
        this._verifyDocumentsExist(docs, collection);

        // Do not drop the collection if this._skipCollectionSetup because the caller owns the
        // setup/teardown of the collection.
        if (!this._skipCollectionSetup) {
            this._mongos.getDB(this._dbName)[this._collName].drop();
        }

        this._verifyAllShardingCollectionsRemoved(tempReshardingCollName);
    }

    /**
     * Run reshardCollection and check the number of chunks is as expected.
     * @param {Object} commandObj The reshardCollection cmd to execute.
     * @param {Number} expectedChunkNum Number of chunks to have after reshardCollection.
     * @param {Object[]} expectedChunks Expected chunk distribution after reshardCollection.
     * @param {Object[]} expectedZones Expected zones for the collection after reshardCollection.
     * @param {Function} additionalSetup Additional setup needed, taking the class object as input.
     */
    assertReshardCollOk(
        commandObj, expectedChunkNum, expectedChunks, expectedZones, additionalSetup) {
        if (!this._skipCollectionSetup) {
            const oldShardKey = {oldKey: 1};
            assert.commandWorked(
                this._mongos.adminCommand({shardCollection: this._ns, key: oldShardKey}));
            this._verifyShardKey(oldShardKey);
        }

        let docs = [];
        let collection = this._mongos.getDB(this._dbName).getCollection(this._collName);
        let bulk = collection.initializeOrderedBulkOp();
        for (let x = 0; x < this._numInitialDocs; x++) {
            let doc = {oldKey: x, newKey: this._numInitialDocs - x};
            docs.push(doc);
            bulk.insert(doc);
        }
        assert.commandWorked(bulk.execute());
        if (additionalSetup) {
            additionalSetup(this);
        }

        const indexes = this._mongos.getDB(this._dbName).getCollection(this._collName).getIndexes();
        const tempReshardingCollName =
            this._constructTemporaryReshardingCollName(this._dbName, this._collName);

        assert.commandWorked(this._mongos.adminCommand(commandObj));

        this._verifyShardKey(commandObj.key);

        if (expectedChunks) {
            this._verifyTemporaryReshardingCollectionExistsWithCorrectOptions(
                commandObj.key, this._getAllShardIdsFromExpectedChunks(expectedChunks));
        }

        this._verifyTagsDocumentsAfterOperationCompletes(
            this._ns, Object.keys(commandObj.key), expectedZones);

        this._verifyChunksMatchExpected(expectedChunkNum, expectedChunks);

        this._verifyIndexesCreated(indexes, commandObj.key);

        this._verifyDocumentsExist(docs, collection);

        // Do not drop the collection if this._skipCollectionSetup because the caller owns the
        // setup/teardown of the collection.
        if (!this._skipCollectionSetup) {
            this._mongos.getDB(this._dbName)[this._collName].drop();
        }

        this._verifyAllShardingCollectionsRemoved(tempReshardingCollName);
    }
}
