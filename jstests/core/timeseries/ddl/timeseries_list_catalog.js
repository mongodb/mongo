/**
 * Tests that $listCatalog stage works with time-series collections.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # $listCatalog will not prefix namespaces in its responses, and therefore is incompatible
 *   # with prefix checking.
 *   simulate_atlas_proxy_incompatible,
 *   # In a multitenancy environment the catalog will always return tenant-prefixed entries, and the
 *   # override we use in the multitenancy suites hecks for the absence of this prefix.
 *   simulate_mongoq_incompatible,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {
    areViewlessTimeseriesEnabled,
    getTimeseriesCollForDDLOps
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

assert.commandWorked(db.dropDatabase());

const collName = 'coll';
const timeFieldName = 'timestamp';
const metaFieldName = 'metadata';

function validateCollectionMetadata(coll, metadata, onAdminDB) {
    jsTest.log(`Context | nss: ${coll.getFullName()}, tracked: ${coll._isSharded()}, onAdminDB: ${
        onAdminDB}`);
    jsTest.log(`Collection metadata: ${tojson(metadata)}`);
    const viewlessTimeseriesEnabled = areViewlessTimeseriesEnabled(db);
    // TODO SERVER-103776 remove `onAdminDB` from the following condition once the bug is fixed.
    if (viewlessTimeseriesEnabled && onAdminDB) {
        assert.eq('timeseries', metadata.type);
    } else {
        assert.eq('collection', metadata.type);
    }
    assert.hasFields(metadata.md.options, ['timeseries']);
    const tsOptions = metadata.md.options.timeseries;

    assert.hasFields(tsOptions, ['timeField', 'metaField']);

    assert.eq(timeFieldName, tsOptions.timeField);
    assert.eq(metaFieldName, tsOptions.metaField);
}

function validateListCatalog(coll, listCatalogRes, onAdminDB, collExists) {
    if (!collExists) {
        assert.eq(0,
                  listCatalogRes.length,
                  `Expected listCatalog output to be empty but found: ${tojson(listCatalogRes)}`);
        return;
    }

    if (!coll._isSharded()) {
        assert.eq(
            1,
            listCatalogRes.length,
            `Expected listCatalog to have exactly one entry for unsharded timeseries collection '${
                collName}' but found: ${tojson(listCatalogRes)}`);
        validateCollectionMetadata(coll, listCatalogRes[0], onAdminDB);
    } else {
        listCatalogRes.forEach(listCatalogEntry =>
                                   validateCollectionMetadata(coll, listCatalogEntry, onAdminDB));
    }
}
function testListCatalog(coll, collExists) {
    // Check output of listCatalog on admin namespace
    let listCatalogRes =
        coll.getDB()
            .getSiblingDB('admin')
            .aggregate([
                {$listCatalog: {}},
                {
                    $match: {
                        'db': coll.getDB().getName(),
                        'name': getTimeseriesCollForDDLOps(coll.getDB(), coll).getName()
                    }
                }
            ])
            .toArray();
    validateListCatalog(getTimeseriesCollForDDLOps(coll.getDB(), coll),
                        listCatalogRes,
                        true /* onAdminDB */,
                        collExists);

    // Check output of listCatalog on collection namespace
    listCatalogRes =
        getTimeseriesCollForDDLOps(coll.getDB(), coll).aggregate([{$listCatalog: {}}]).toArray();
    validateListCatalog(getTimeseriesCollForDDLOps(coll.getDB(), coll),
                        listCatalogRes,
                        false /* onAdminDB */,
                        collExists);
}

const coll = db[collName];
testListCatalog(coll, false /* collExists */);

assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
testListCatalog(coll, true /* collExists */);

TimeseriesTest.insertManyDocs(coll);
testListCatalog(coll, true /* collExists */);

coll.drop();
testListCatalog(coll, false /* collExists */);
