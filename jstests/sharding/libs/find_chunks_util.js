import {findTimeseriesConfigCollectionsDocument} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

/*
 * Utilities for looking up chunk metadata
 */
export var findChunksUtil = (function () {
    /**
     * Performs a find() on config.chunks on 'configDB', targeting chunks for the collection 'ns',
     * and the optional 'extraQuery' and 'projection'.
     */
    let findChunksByNs = function (configDB, ns, extraQuery = null, projection = null) {
        let collection = configDB.collections.findOne({_id: ns});

        // TODO SERVER-101609 remove once 9.0 becomes last LTS
        if (!collection && configDB.getMongo().getCollection(ns).getMetadata().type === "timeseries") {
            collection = findTimeseriesConfigCollectionsDocument(configDB.getMongo().getCollection(ns));
        }

        assert.neq(collection.uuid, null);
        const chunksQuery = Object.assign({uuid: collection.uuid}, extraQuery);
        return configDB.chunks.find(chunksQuery, projection);
    };

    /**
     * Performs a findOne() on config.chunks on 'configDB', targeting chunks for the collection
     * 'ns', and the optional 'extraQuery' and 'projection'. Chooses to query chunks by their 'ns'
     * or uuid' fields according to it's config.collection entry having 'timestamp' or not.
     */
    let findOneChunkByNs = function (configDB, ns, extraQuery = null, projection = null) {
        const q = findChunksByNs(configDB, ns, extraQuery, projection).limit(1);
        return q.hasNext() ? q.next() : null;
    };

    /**
     * Performs a count() on config.chunks on 'configDB', targeting chunks for the collection 'ns',
     * and the optional 'extraQuery' and 'projection'.
     * Chooses to query chunks by their 'ns' or uuid' fields according to it's config.collection
     * entry having 'timestamp' or not.
     */
    let countChunksForNs = function (configDB, ns, extraQuery = null) {
        return findChunksByNs(configDB, ns, extraQuery).count();
    };

    /**
     * Returns the appropriate chunks join clause for collection 'ns'.
     */
    let getChunksJoinClause = function (configDB, ns) {
        const collMetadata = configDB.collections.findOne({_id: ns});
        return {uuid: collMetadata.uuid};
    };

    return {
        findChunksByNs,
        findOneChunkByNs,
        countChunksForNs,
        getChunksJoinClause,
    };
})();
