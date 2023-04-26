/*
 * Utilities for looking up chunk metadata
 */
var findChunksUtil = (function() {
    /**
     * Performs a find() on config.chunks on 'configDB', targeting chunks for the collection 'ns',
     * and the optional 'extraQuery' and 'projection'.
     * Chooses to query chunks by their 'ns' or uuid' fields according to it's config.collection
     * entry having 'timestamp' or not.
     */
    let findChunksByNs = function(configDB, ns, extraQuery = null, projection = null) {
        const collection = configDB.collections.findOne({_id: ns});
        if (collection.timestamp) {
            const collectionUUID = configDB.collections.findOne({_id: ns}).uuid;
            assert.neq(collectionUUID, null);
            const chunksQuery = Object.assign({uuid: collectionUUID}, extraQuery);
            return configDB.chunks.find(chunksQuery, projection);
        } else {
            const chunksQuery = Object.assign({ns: ns}, extraQuery);
            return configDB.chunks.find(chunksQuery, projection);
        }
    };

    /**
     * Performs a findOne() on config.chunks on 'configDB', targeting chunks for the collection
     * 'ns', and the optional 'extraQuery' and 'projection'. Chooses to query chunks by their 'ns'
     * or uuid' fields according to it's config.collection entry having 'timestamp' or not.
     */
    let findOneChunkByNs = function(configDB, ns, extraQuery = null, projection = null) {
        const collection = configDB.collections.findOne({_id: ns});
        if (collection.timestamp) {
            const collectionUUID = configDB.collections.findOne({_id: ns}).uuid;
            assert.neq(collectionUUID, null);
            const chunksQuery = Object.assign({uuid: collectionUUID}, extraQuery);
            return configDB.chunks.findOne(chunksQuery, projection);
        } else {
            const chunksQuery = Object.assign({ns: ns}, extraQuery);
            return configDB.chunks.findOne(chunksQuery, projection);
        }
    };

    /**
     * Performs a count() on config.chunks on 'configDB', targeting chunks for the collection 'ns',
     * and the optional 'extraQuery' and 'projection'.
     * Chooses to query chunks by their 'ns' or uuid' fields according to it's config.collection
     * entry having 'timestamp' or not.
     */
    let countChunksForNs = function(configDB, ns, extraQuery = null) {
        return findChunksByNs(configDB, ns, extraQuery).count();
    };

    /**
     * Returns the appropriate chunks join clause for collection 'ns'.
     */
    let getChunksJoinClause = function(configDB, ns) {
        const collMetadata = configDB.collections.findOne({_id: ns});
        if (collMetadata.timestamp) {
            return {uuid: collMetadata.uuid};
        } else {
            return {ns: collMetadata._id};
        }
    };

    return {
        findChunksByNs,
        findOneChunkByNs,
        countChunksForNs,
        getChunksJoinClause,
    };
})();
