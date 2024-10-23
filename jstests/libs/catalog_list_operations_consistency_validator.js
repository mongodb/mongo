/**
 * Implements a consistency checker between:
 * * The results of the `$listCatalog` aggregation.
 * * The results of the `listCollections` command.
 * * The results of the `listIndexes` command.
 *
 * The primary objective of this checker is to ensure that all relevant fields from `$listCatalog`
 * are exposed in `listCollections`' or `listIndexes`' command results.
 * Because `listCollections` and `listIndexes` are the stable interfaces used to implement basic
 * functionality (initial sync, chunk migration, resharding, `moveCollection`, mongodump...),
 * failing to include a required catalog field in their output can result in data loss.
 *
 * To achieve this:
 * * We carefully destructure `$listCatalog`'s results field by field,
 *   and cause an assertion if we find any field we don't recognize.
 * * We explicitly discard those fields which we know are not required for collection cloning
 *   (e.g. ident names, index build information, etc.).
 * * We map all other fields to their equivalent in `listCollections` and `listIndexes`.
 *   That is, we reimplement `listCollections` and `listIndexes` in terms of `$listCatalog`.
 * * We ensure that the computed output from `$listCatalog` matches the actual output from
 *   `listCollections` and `listIndexes` (modulo ordering differences).
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

function assertNoUnrecognizedFields(restParameters, sourceObject, targetObject) {
    assert(
        !Object.keys(restParameters).length,
        `The ${sourceObject} contains unrecognized fields: ${Object.keys(restParameters)}. ` +
            `Please check whether they should be mapped to a field in ${targetObject}, ` +
            "which is essential for collection cloning during initial sync, resharding, and so on. " +
            "See SERVER-90768 for more information.");
}

function mapListCatalogToListCollectionsEntry(listCatalogEntry, listCatalogMap, isDbReadOnly) {
    // Destructure the `$listCatalog` entry and validate that we recognize all fields.
    const {
        name: nsName,
        type: nsType,
        // Redundant namespace information can be safely thrown away.
        db: _ns1,
        ns: _ns2,
        // Origin shard can be safely thrown away, as it is not part of the catalog.
        shard: _ns3,
        ...nsRest
    } = listCatalogEntry;

    if (nsType === 'collection') {
        const {
            md: collMd,
            // Ident information can be safely thrown away.
            ident: _coll1,
            idxIdent: _coll2,
            ...collUnrecognized
        } = nsRest;
        assertNoUnrecognizedFields(
            collUnrecognized, "`$listCatalog` collection entry", "listCollections");

        // Destructure the nested `md` field and validate that we recognize all fields.
        const {
            options: mdOptions,
            // Indexes are carefully checked when validating `listIndexes` later.
            indexes: mdIndexes,
            // Namespace information can be safely thrown away.
            ns: _md1,
            // Non-default values of those fields are stored into WiredTiger's connection string
            // (`md.options.storageEngine.wiredTiger.configString`) `app_metadata` field.
            // See SERVER-91193, SERVER-91194, SERVER-91195, SERVER-92265.
            // Therefore, those fields do not need to be returned in `listCollections`' output.
            timeseriesBucketsMayHaveMixedSchemaData: _md2,
            timeseriesBucketingParametersHaveChanged: _md3,
            // This field does not exist anymore since MongoDB 5.0 but it's possible to observe
            // it in multiversion tests. See SERVER-28742 (added), SERVER-53982 (removed).
            prefix: _md4,
            ...mdUnrecognized
        } = collMd;
        assertNoUnrecognizedFields(
            mdUnrecognized, "`md` field from `listCatalog` entry", "listCollection");

        // The `_id` index is included in the `idIndex` field from `listCollections`.
        // Note that clustered or capped collections do not have an id index.
        const idIndex = mdIndexes.find(k => k.spec.name === "_id_");

        // The `uuid` from `md.options` is moved to the `info` field from `listCollections`.
        const {uuid: mdOptionsUuid, ...mdOptionsRest} = mdOptions;

        return {
            name: nsName,
            type: nsType,
            options: mdOptionsRest,
            info: {readOnly: isDbReadOnly, uuid: mdOptionsUuid},
            ...(idIndex !== undefined && {idIndex: idIndex.spec}),
        };
    } else if (nsType === 'view') {
        const {
            _id: _view1,
            viewOn: viewViewOn,
            pipeline: viewPipeline,
            collation: viewCollation,
            ...viewUnrecognized
        } = nsRest;
        assertNoUnrecognizedFields(
            viewUnrecognized, "`$listCatalog` view entry", "listCollections");

        return {
            name: nsName,
            type: nsType,
            options: {
                viewOn: viewViewOn,
                pipeline: viewPipeline,
                ...(viewCollation !== undefined && {collation: viewCollation}),
            },
            info: {readOnly: true},
        };
    } else if (nsType === 'timeseries') {
        const {
            _id: _ts1,
            viewOn: tsViewOn,
            // This information is redundant with the buckets collection.
            pipeline: _ts2,
            collation: _ts3,
            ...tsUnrecognized
        } = nsRest;
        assertNoUnrecognizedFields(
            tsUnrecognized, "`$listCatalog` timeseries entry", "listCollections");

        // For time series collections, the options are those of the buckets collection.
        // `listCollections` only lists the options provided by the user to `createCollection`.
        // Here, where we explicitly list the options that MongoDB internally sets in the
        // bucket collection, and assume the rest are the user-provided options.
        const tsBucketsCollection = listCatalogMap.get(tsViewOn);
        const {
            uuid: _md1,
            validator: _md2,
            clusteredIndex: _md3,
            ...tsBucketsCollectionOptionsRest
        } = tsBucketsCollection.md.options;

        return {
            name: nsName,
            type: nsType,
            options: tsBucketsCollectionOptionsRest,
            info: {readOnly: isDbReadOnly},
        };
    } else {
        assert(false, `Unknown namespace type ${nsType} for namespace ${nsName}.`);
    }
}

function mapListCatalogToListIndexesEntry(listCatalogEntry) {
    const indexes = listCatalogEntry.md.indexes.map(mdIndex => {
        const {
            spec: mdIndexSpec,
            // Index build status information can safely be thrown away.
            ready: _1,
            backgroundSecondary: _2,
            buildUUID: _3,
            // Multikey information is determined by the index spec. plus observed documents.
            // It may be inconsistent among replica set members (e.g. a primary may think that
            // an index is multikey, but a secondary added later may not, if it has never
            // observed documents with indexed array fields). Those inconsistencies are
            // acceptable, since they are pessimizing, and thus correctness is guaranteed.
            multikey: _4,
            multikeyPaths: _5,
            // This is a legacy field from the MMAP engine which is currently unused (always 0).
            head: _6,
            // This field does not exist anymore since MongoDB 5.0 but it's possible to observe
            // it in multiversion tests. See SERVER-28742 (added), SERVER-53982 (removed).
            prefix: _7,
            // Those fields do not exist anymore since MongoDB 4.4 but it's possible to observe
            // them in multiversion tests. See SERVER-37645 (added), SERVER-44428 (removed).
            runTwoPhaseBuild: _8,
            versionOfBuild: _9,
            ...mdIndexesUnrecognized
        } = mdIndex;

        assertNoUnrecognizedFields(
            mdIndexesUnrecognized, "`md.indexes` field from `$listCatalog` entry", "listIndexes");

        const {
            // This field does not exist anymore since MongoDB 4.3 but it's possible to observe
            // it in multiversion tests. See SERVER-41696 (removed).
            ns: _10,
            ...mdIndexSpecRest
        } = mdIndexSpec;

        return {
            ...mdIndexSpecRest,
            // Handle various mapping quirks of `listIndexes` vs the raw catalog. See also:
            // https://github.com/mongodb/mongo/blob/96893c43d288b84fccb4242b51c8d7c57df5887d/src/mongo/db/catalog/README.md#examples-of-differences-between-listindexes-and-listcatalog-results
            ...(mdIndexSpec.background === 1 && {background: true}),
            ...(typeof mdIndexSpec.sparse === "number" && {sparse: mdIndexSpec.sparse !== 0}),
            ...(typeof mdIndexSpec.bits === "number" && {bits: Math.floor(mdIndexSpec.bits)}),
            // For more details on the history and handling of NaN on TTL indexes, see:
            // https://www.mongodb.com/docs/v8.0/tutorial/expire-data/#indexes-configured-using-nan
            ...(typeof mdIndexSpec.expireAfterSeconds === "number" && {
                expireAfterSeconds: !isNaN(mdIndexSpec.expireAfterSeconds)
                    ? Math.floor(mdIndexSpec.expireAfterSeconds)
                    : 2147483647
            }),
        };
    });

    const mdOptions = listCatalogEntry.md.options;

    // Clustered indexes are not stored in the `indexes` field of the catalog, but rather as
    // part of the collection options. However, they are returned by `listIndexes`.
    if ((typeof mdOptions.clusteredIndex === "object" || mdOptions.clusteredIndex === true) &&
        !mdOptions.timeseries) {
        indexes.push({
            ...(typeof mdOptions.clusteredIndex === "object" ? mdOptions.clusteredIndex : {
                // `{clusteredIndex: true}` legacy index format means a canonical _id index.
                v: 2,
                key: {
                    _id: 1,
                },
                name: "_id_",
                unique: true
            }),
            ...(mdOptions.collation !== undefined && {collation: mdOptions.collation}),
            ...(mdOptions.expireAfterSeconds !== undefined &&
                {expireAfterSeconds: mdOptions.expireAfterSeconds}),
            clustered: true
        });
    }

    return {name: listCatalogEntry.name, indexes};
}

/**
 * Removes duplicates from a list of BSON documents.
 * Assumes all duplicates in the list are adjacent (e.g. because the list is sorted).
 *
 * This check handles the fact that `$listCatalog` returns a document per collection and shard,
 * while `listCollections` only returns one per collection (and similarly for `listIndexes`).
 */
function removeDuplicateDocuments(docList) {
    return docList.filter((item, index) =>
                              index === 0 || bsonWoCompare(docList[index - 1], item) !== 0);
}

function validateListCatalogToListCollectionsConsistency(
    listCatalog, listCollections, isDbReadOnly, doAssert) {
    // Sorting function to ignore irrelevant ordering differences while comparing.
    function sortCollectionsInPlace(collectionList) {
        return collectionList.sort((a, b) => a.name.localeCompare(b.name));
    }

    // Create a map for looking up $listCatalog namespaces by name. We use this for time series
    // collections, which inherit their options from their buckets collection in listCollections.
    const listCatalogMap = new Map(listCatalog.map(c => [c.name, c]));

    const listCollectionsFromListCatalog =
        removeDuplicateDocuments(sortCollectionsInPlace(listCatalog.map(
            ci => mapListCatalogToListCollectionsEntry(ci, listCatalogMap, isDbReadOnly))));
    const sortedListCollections = sortCollectionsInPlace([...listCollections]);

    const equals = bsonWoCompare(listCollectionsFromListCatalog, sortedListCollections) === 0;
    if (!equals) {
        const message = "$listCatalog to listCollections consistency check failed.\n" +
            "expected (from $listCatalog): " + tojson(listCollectionsFromListCatalog) + "\n" +
            "actual (listCollections): " + tojson(sortedListCollections) + "\n" +
            "full $listCatalog output: " + tojson(listCatalog);
        if (doAssert === false) {
            print(message);
        } else {
            assert(equals, message);
        }
    }
    return equals;
}

function validateListCatalogToListIndexesConsistency(listCatalog, listIndexes, doAssert) {
    // Sorting function to ignore irrelevant ordering differences while comparing.
    function sortCollectionIndexesInPlace(indexList) {
        indexList.sort((a, b) => a.name.localeCompare(b.name));  // Sort by collection.
        indexList.forEach(  // Sort the indexes of each collection.
            ci => ci.indexes.sort((ia, ib) => ia.name.localeCompare(ib.name)));
        return indexList;
    }

    // Some tests create a large amount of collections or indexes, and/or very bloated indexes,
    // so that comparing it all at once fails due to it exceeding the BSON document size limit.
    // This works around the issue by running the comparison element by element.
    function bsonUnorderedFieldArrayEquals(a, b) {
        return a.length === b.length &&
            a.every((item, index) => bsonUnorderedFieldsCompare(item, b[index]) === 0);
    }

    const listIndexesFromListCatalog = removeDuplicateDocuments(sortCollectionIndexesInPlace(
        listCatalog.filter(lc => lc.type === 'collection').map(mapListCatalogToListIndexesEntry)));
    const sortedListIndexes = sortCollectionIndexesInPlace(
        // Shallow-copy to avoid modifying the list given by the caller.
        listIndexes.map(ci => ({name: ci.name, indexes: [...ci.indexes]})));

    const equals = bsonUnorderedFieldArrayEquals(listIndexesFromListCatalog, sortedListIndexes);
    if (!equals) {
        const message = "$listCatalog to listIndexes consistency check failed.\n" +
            "expected (from $listCatalog): " + tojson(listIndexesFromListCatalog) + "\n" +
            "actual (listIndexes): " + tojson(sortedListIndexes) + "\n" +
            "full $listCatalog output: " + tojson(listCatalog);
        if (doAssert === false) {
            print(message);
        } else {
            assert(equals, message);
        }
    }
    return equals;
}

export function validateCatalogListOperationsConsistency(
    listCatalog, listCollections, listIndexes, isDbReadOnly, doAssert) {
    return validateListCatalogToListCollectionsConsistency(
               listCatalog, listCollections, isDbReadOnly, doAssert) &&
        validateListCatalogToListIndexesConsistency(listCatalog, listIndexes, doAssert);
}

/**
 * Run the `$listCatalog` / `listCollections` / `listIndexes` consistency checker over a collection.
 * This assumes that no operations are running over the collection
 * (as otherwise, each operation may observe the collection at a different point in time).
 */
export function assertCatalogListOperationsConsistencyForCollection(collection) {
    // For time series collections, also check their associated buckets namespace
    const collectionNames = collection.exists().type === "timeseries"
        ? [collection.getName(), "system.buckets." + collection.getName()]
        : [collection.getName()];

    // The database is read only when using standalone recovery options like --queryableBackupMode,
    // so we can assume that the database is not read-only when running a sharded cluster
    const isDbReadOnly = !FixtureHelpers.isMongos(db) && db.serverStatus().storageEngine.readOnly;

    const listCollections = db.getCollectionInfos({name: {$in: collectionNames}});
    const listIndexes =
        listCollections
            // Note that while time series collections have indexes from the user's point of view,
            // at the catalog level they only exist in the associated buckets collection.
            .filter(c => c.type === 'collection')
            .map(coll => ({name: coll.name, indexes: db.getCollection(coll.name).getIndexes()}));

    let listCatalog =
        db.getSiblingDB("admin")
            .aggregate(
                [{$listCatalog: {}}, {$match: {db: db.getName(), name: {$in: collectionNames}}}])
            .toArray();

    // Collectionless $listCatalog can return entries from shards that do not own any chunk.
    // This is problematic because those shards can be stale (for example, not have indexes that
    // were created while they do not own any chunk) and thus fail the consistency check.
    // As a workaround, re-run a 'collectionful' $listCatalog on each collection to remove them
    // TODO(SERVER-64980): Remove once collectionless $listCatalog removes shards without chunks
    const collectionsFromListCatalog =
        [...new Set(listCatalog.filter(e => e.type === 'collection').map(e => e.name))];
    const refetchedListCatalog = collectionsFromListCatalog.flatMap(
        collName => db.getCollection(collName).aggregate([{$listCatalog: {}}]).toArray());
    listCatalog = listCatalog.filter(e => e.type !== 'collection').concat(refetchedListCatalog);

    validateCatalogListOperationsConsistency(
        listCatalog, listCollections, listIndexes, isDbReadOnly, true);
}
