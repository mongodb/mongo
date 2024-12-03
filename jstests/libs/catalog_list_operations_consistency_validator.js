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
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

function assertNoUnrecognizedFields(restParameters, sourceName, targetName, sourceObject) {
    assert(
        !Object.keys(restParameters).length,
        `The ${sourceName} contains unrecognized fields: ${Object.keys(restParameters)}. ` +
            `Please check whether they should be mapped to a field in ${targetName}, ` +
            "which is essential for collection cloning during initial sync, resharding, and so on. " +
            "See SERVER-90768 for more information. Source object: " + tojson(sourceObject));
}

function mapListCatalogToListCollectionsEntry(listCatalogEntry, listCatalogMap, isDbReadOnly) {
    // Destructure the `$listCatalog` entry and validate that we recognize all fields.
    const {
        name: nsName,
        type: nsType,
        // Note that this field can come on both collections/views/timeseries on $listCatalog,
        // but is only mapped onto the response of listCollections for collections.
        configDebugDump: nsConfigDebugDump,
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
        assertNoUnrecognizedFields(collUnrecognized,
                                   "`$listCatalog` collection entry",
                                   "listCollections",
                                   listCatalogEntry);

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
        assertNoUnrecognizedFields(mdUnrecognized,
                                   "`md` field from `listCatalog` entry",
                                   "listCollection",
                                   listCatalogEntry);

        // The `_id` index is included in the `idIndex` field from `listCollections`.
        // Note that clustered or capped collections do not have an id index.
        const idIndex = mdIndexes.find(k => k.spec.name === "_id_");

        // The `uuid` from `md.options` is moved to the `info` field from `listCollections`.
        const {uuid: mdOptionsUuid, ...mdOptionsRest} = mdOptions;

        return {
            name: nsName,
            type: nsType,
            options: mdOptionsRest,
            info: {
                readOnly: isDbReadOnly,
                uuid: mdOptionsUuid,
                ...(nsConfigDebugDump !== undefined && {configDebugDump: nsConfigDebugDump})
            },
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
            viewUnrecognized, "`$listCatalog` view entry", "listCollections", listCatalogEntry);

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
            tsUnrecognized, "`$listCatalog` timeseries entry", "listCollections", listCatalogEntry);

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

        assertNoUnrecognizedFields(mdIndexesUnrecognized,
                                   "`md.indexes` field from `$listCatalog` entry",
                                   "listIndexes",
                                   listCatalogEntry);

        // Certain options intended for plugins such as 2d, text, etc.. are allowed by createIndex
        // and persisted in the catalog, but don't appear in listCollections' output. Remove them.
        // TODO(SERVER-97084): Remove when options for index plugins are denied in basic indexes.
        const indexPlugin = Object.values(mdIndexSpec.key).find(x => typeof x === "string");
        if (indexPlugin !== "2dsphere" && indexPlugin !== "2dsphere_bucket") {
            delete mdIndexSpec["2dsphereIndexVersion"];
        }
        if (indexPlugin !== "2d") {
            delete mdIndexSpec.bits;
            delete mdIndexSpec.min;
            delete mdIndexSpec.max;
        }
        if (indexPlugin !== "text") {
            delete mdIndexSpec.textIndexVersion;
        }

        const {
            // This field does not exist anymore since MongoDB 4.3 but it's possible to observe
            // it in multiversion tests. See SERVER-41696 (removed).
            ns: _10,
            // TODO (SERVER-97749) Stop ignoring the `background` field once it's properly handled.
            background: _11,
            ...mdIndexSpecRest
        } = mdIndexSpec;

        return {
            ...mdIndexSpecRest,
            // TODO (SERVER-97749) Add the background field once it's properly handled.
            // Handle various mapping quirks of `listIndexes` vs the raw catalog. See also:
            // https://github.com/mongodb/mongo/blob/96893c43d288b84fccb4242b51c8d7c57df5887d/src/mongo/db/catalog/README.md#examples-of-differences-between-listindexes-and-listcatalog-results
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
    listCatalog, listCollections, isDbReadOnly, shouldAssert) {
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
        if (shouldAssert !== false) {
            doassert(message);
        }
        print(message);
    }
    return equals;
}

function validateListCatalogToListIndexesConsistency(listCatalog, listIndexes, shouldAssert) {
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
        listIndexes.map(ci => ({
                            name: ci.name,
                            indexes: [...ci.indexes.map(index => {
                                // TODO (SERVER-97749): Don't delete 'background' field once we
                                // handle it properly
                                delete index.background;
                                return index;
                            })]
                        })));

    const equals = bsonUnorderedFieldArrayEquals(listIndexesFromListCatalog, sortedListIndexes);
    if (!equals) {
        const message = "$listCatalog to listIndexes consistency check failed.\n" +
            "expected (from $listCatalog): " + tojson(listIndexesFromListCatalog) + "\n" +
            "actual (listIndexes): " + tojson(sortedListIndexes) + "\n" +
            "full $listCatalog output: " + tojson(listCatalog);
        if (shouldAssert !== false) {
            doassert(message);
        }
        print(message);
    }
    return equals;
}

function validateCatalogListOperationsConsistency(
    listCatalog, listCollections, listIndexes, isDbReadOnly, shouldAssert) {
    return validateListCatalogToListCollectionsConsistency(
               listCatalog, listCollections, isDbReadOnly, shouldAssert) &&
        validateListCatalogToListIndexesConsistency(listCatalog, listIndexes, shouldAssert);
}

// TODO(SERVER-64980): Remove once collectionless $listCatalog removes shards without chunks.
function filterListCatalogEntriesFromShardsWithoutChunks(db, listCatalog) {
    // Collectionless $listCatalog can return entries from shards that do not own any chunk.
    // This is problematic because those shards can be stale (for example, not have indexes that
    // were created while they do not own any chunk) and thus make the consistency check fail.
    // In contrast, $listCatalog over a specific collection follows the Shard Versioning
    // Protocol, and therefore only returns non-stale entries from shards that own chunks.
    // We re-run a 'collectionful' $listCatalog on each collection to clean up stale entries.
    const collectionsFromListCatalog =
        [...new Set(listCatalog.filter(e => e.type === 'collection').map(e => e.name))];
    const refetchedListCatalog = collectionsFromListCatalog.flatMap(
        collName => db.getCollection(collName).aggregate([{$listCatalog: {}}]).toArray());
    return listCatalog.filter(e => e.type !== 'collection').concat(refetchedListCatalog);
}

/**
 * Run the `$listCatalog` / `listCollections` / `listIndexes` consistency checker over a collection.
 * This assumes that no operations are running over the collection
 * (as otherwise, each operation may observe the collection at a different point in time).
 */
export function assertCatalogListOperationsConsistencyForCollection(collection) {
    // For time series collections, also check their associated buckets namespace.
    const collectionNames = collection.exists().type === "timeseries"
        ? [collection.getName(), "system.buckets." + collection.getName()]
        : [collection.getName()];

    // The database is read only when using standalone recovery options like --queryableBackupMode,
    // so we can assume that the database is not read-only when running a sharded cluster.
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

    listCatalog = filterListCatalogEntriesFromShardsWithoutChunks(db, listCatalog);

    validateCatalogListOperationsConsistency(
        listCatalog, listCollections, listIndexes, isDbReadOnly, true);
}

export function assertCatalogListOperationsConsistencyForDb(db, tenantId) {
    const isMongos = FixtureHelpers.isMongos(db);

    function getCatalogInfoIfAvailable(filter) {
        const acceptableErrorCodes = [
            // Happens on api_strict suites since $listCatalog is not part of the stable API.
            ErrorCodes.APIStrictError,
            // Happens on multiversion suites as $listCatalog only exists since MongoDB 6.0.
            40324,  // "Unrecognized pipeline stage name: ..."
            // Happens on tests that launch mongod with a very low `maxBSONDepth` server param.
            5729100,  // "FieldPath is too long"
        ];
        try {
            const nsPrefix = (tenantId ? tenantId + "_" + db.getName() : db.getName()) + ".";
            let listCatalog =
                db.getSiblingDB("admin")
                    .aggregate([
                        {$listCatalog: {}},
                        {
                            $match:
                                {...filter, ns: {$regex: new RegExp(`^${RegExp.escape(nsPrefix)}`)}}
                        }
                    ])
                    .toArray();

            // We don't need to call `filterListCatalogEntriesFromShardsWithoutChunks` here,
            // as currently the only part the catalog that is inconsistent in shards without chunks
            // are indexes, and we already skip validating those due to SERVER-75675.

            return listCatalog;
        } catch (ex) {
            if (acceptableErrorCodes.includes(ex.code)) {
                return null;
            }
            throw ex;
        }
    }

    // For the `listCollections` to `$listCatalog` consistency check, we need to know if the DB
    // is read-only, as `listCollections` reports this information even though it is not part
    // of the catalog, so we need to fetch it separately.
    const isDbReadOnly = !isMongos &&
        assert
            .commandWorked(db.serverStatus({
                // This server status section attempts to get WiredTiger storage size statistics,
                // and fails if a validate is in progress, similarly to the ObjectIsBusy error
                // below. Since we don't need it, we can just exclude it from the output.
                changeStreamPreImages: 0
            }))
            .storageEngine.readOnly;

    // Don't check these DBs on mongos since it will mirror them from the config server for
    // listCollections & listIndexes, but will return the data from the shards on $listCatalog.
    if (isMongos && ["admin", "config"].includes(db.getName()))
        return;

    print("Running catalog operations consistency check for DB " + db.getName() +
          (tenantId ? " of tenant " + tenantId : ""));

    let consistencyCheckAttempts = 0;
    assert.soon(() => {
        let collInfo, catalogInfo, collIndexes;
        try {
            let filter = undefined; /* Consider everything (collections, views, timeseries) */
            if (jsTest.options().skipValidationOnInvalidViewDefinitions) {
                // If skipValidationOnInvalidViewDefinitions=true, then we avoid resolving the view
                // catalog on the admin database.
                //
                // TODO SERVER-25493: Remove the $exists clause once performing an initial sync from
                // versions of MongoDB <= 3.2 is no longer supported.
                filter = {$or: [{type: "collection"}, {type: {$exists: false}}]};
            } else if (tenantId && FeatureFlagUtil.isEnabled(db, "RequireTenantID")) {
                // Views (including time series) do not include the tenant prefix in the catalog
                // if featureFlagRequireTenantID is enabled, thus they can't be filtered by tenant
                // based on $listCatalog's output. See SERVER-85199 for more details.
                filter = {type: "collection"};
            }
            collInfo = db.getCollectionInfos(filter);
            catalogInfo = getCatalogInfoIfAvailable(filter);
            // TODO SERVER-75675: do not skip index consistency check on sharded clusters.
            collIndexes = (catalogInfo && !isMongos)
                ? collInfo.filter(coll => coll.type === "collection")
                      .map(
                          coll => (
                              {name: coll.name, indexes: db.getCollection(coll.name).getIndexes()}))
                : null;
        } catch (ex) {
            // In a sharded cluster with in-progress validate command for the config database
            // (i.e. on the config server), catalog accesses on a mongos or shardsvr mongod that
            // has stale routing info may fail since a refresh would involve running read commands
            // against the config database. The read commands are lock free so they are not blocked
            // by the validate command and instead are subject to failing with a ObjectIsBusy error.
            // Since this is a transient state, we should retry.
            if (ex.code === ErrorCodes.ObjectIsBusy) {
                return false;
            }
            // When the `listIndexes` command goes through the sharding code (for example, on a
            // replica set endpoint), the command handler forwards the command to the shard with the
            // MinKey chunk, which may require refreshing the collection routing information.
            // If this happens while the config server is ongoing a state transition, it will fail
            // with this code. Since this is a transient state, we should retry.
            if (ex.code === ErrorCodes.InterruptedDueToReplStateChange) {
                return false;
            }
            throw ex;
        }

        // Replica set endpoints with profiling enabled on an empty database do not show the
        // system.profile collection in listCollections, but they do in $listCatalog.
        // TODO(SERVER-97721): Remove this workaround.
        if (collInfo.length === 0 && catalogInfo !== null && catalogInfo.length === 1 &&
            catalogInfo[0].name === "system.profile" &&
            FeatureFlagUtil.isEnabled(db, "ReplicaSetEndpoint")) {
            print("Skipped consistency check: Stray system.profile on RSEndpoint (SERVER-97721)");
            return true;
        }

        // Check consistency between `listCollections`, `listIndexes` and `$listCatalog` results.
        // It is possible that the two commands return spuriously inconsistent results, for example
        // due to oplog entries for collection drops or renames still being applied to a secondary.
        // So, we may need to retry a few times until we converge to a consistent result set.
        // But, if we repeatedly fail, don't stall the test for too long but fail fast instead.
        const shouldAssert = consistencyCheckAttempts++ > 20;
        if (catalogInfo !== null &&
            !validateListCatalogToListCollectionsConsistency(
                catalogInfo, collInfo, isDbReadOnly, shouldAssert)) {
            print("$listCatalog/listCollections consistency check failed, retrying...");
            return false;
        }
        if (catalogInfo !== null && collIndexes !== null &&
            !validateListCatalogToListIndexesConsistency(catalogInfo, collIndexes, shouldAssert)) {
            print("$listCatalog/listIndexes consistency check failed, retrying...");
            return false;
        }

        return true;
    });
}
