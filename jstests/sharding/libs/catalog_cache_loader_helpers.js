load("jstests/libs/uuid_util.js");

/**
 * Returns true if the passed collection supports long name, either implicitly or explicitly.
 * Otherwise, it returns false.
 */
function supportLongCollectionName(collEntry) {
    return collEntry.supportingLongName == 'implicitly_enabled' ||
        collEntry.supportingLongName == 'explicitly_enabled';
}

/**
 * Returns the name of the chunks collection cached on shards, according to the configuration of the
 * passed collection. If the passed collection supports the long names (implicitly or explicitly),
 * the function returns "cache.chunks.<UUID>". Otherwise, it returns "cache.chunks.<NS>". This is
 * based on the current implementation logic of the Shard Server Catalog Cache Loader.
 */
function getCachedChunksCollectionName(collEntry) {
    return 'cache.chunks.' +
        (supportLongCollectionName(collEntry) ? extractUUIDFromObject(collEntry.uuid)
                                              : collEntry._id);
}
