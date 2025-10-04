/**
 * Tests whether various MongoDB commands automatically upgrade the index version of existing
 * indexes when they are rebuilt on a collection.
 */
import {IndexCatalogHelpers} from "jstests/libs/index_catalog_helpers.js";

let conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");

let testDB = conn.getDB("test");
assert.commandWorked(testDB.runCommand({create: jsTestName()}));
let allIndexes = testDB[jsTestName()].getIndexes();
let spec = IndexCatalogHelpers.findByKeyPattern(allIndexes, {_id: 1});
assert.neq(null, spec, "Index with key pattern {_id: 1} not found: " + tojson(allIndexes));
let defaultIndexVersion = spec.v;
assert.lte(2, defaultIndexVersion, "Expected the defaultIndexVersion to be at least v=2");

/**
 * Tests whether the execution of the 'commandFn' function automatically upgrades the index
 * version of existing indexes.
 *
 * The 'commandFn' function takes a single argument of the collection to act on and returns a
 * collection to validate the index versions of. Most often the 'commandFn' function returns
 * its input collection, but is able to return a reference to a different collection to support
 * testing the effects of cloning commands.
 *
 * If 'doesAutoUpgrade' is true, then this function verifies that the indexes on the returned
 * collection have been upgraded to the 'defaultIndexVersion'. If 'doesAutoUpgrade' is false,
 * then this function verifies that the indexes on the returned collection are unchanged.
 */
function testIndexVersionAutoUpgrades(commandFn, doesAutoUpgrade) {
    testDB.dropDatabase();
    let coll = testDB[jsTestName()];

    // Create a v=1 _id index.
    assert.commandWorked(testDB.createCollection(jsTestName(), {idIndex: {key: {_id: 1}, name: "_id_", v: 1}}));
    let allIndexes = coll.getIndexes();
    let spec = IndexCatalogHelpers.findByKeyPattern(allIndexes, {_id: 1});
    assert.neq(null, spec, "Index with key pattern {_id: 1} not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, "Expected a v=1 index to be built: " + tojson(spec));

    assert.commandWorked(coll.createIndex({withoutAnyOptions: 1}));
    allIndexes = coll.getIndexes();
    spec = IndexCatalogHelpers.findByKeyPattern(allIndexes, {withoutAnyOptions: 1});
    assert.neq(null, spec, "Index with key pattern {withoutAnyOptions: 1} not found: " + tojson(allIndexes));
    assert.eq(defaultIndexVersion, spec.v, "Expected an index with the default version to be built: " + tojson(spec));

    assert.commandWorked(coll.createIndex({withV1: 1}, {v: 1}));
    allIndexes = coll.getIndexes();
    spec = IndexCatalogHelpers.findByKeyPattern(allIndexes, {withV1: 1});
    assert.neq(null, spec, "Index with key pattern {withV1: 1} not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, "Expected a v=1 index to be built: " + tojson(spec));

    assert.commandWorked(coll.createIndex({withV2: 1}, {v: 2}));
    allIndexes = coll.getIndexes();
    spec = IndexCatalogHelpers.findByKeyPattern(allIndexes, {withV2: 1});
    assert.neq(null, spec, "Index with key pattern {withV2: 1} not found: " + tojson(allIndexes));
    assert.eq(2, spec.v, "Expected a v=2 index to be built: " + tojson(spec));

    let collToVerify = commandFn(coll);
    let expectedResults;

    if (doesAutoUpgrade) {
        expectedResults = [
            {keyPattern: {_id: 1}, version: defaultIndexVersion},
            {keyPattern: {withoutAnyOptions: 1}, version: defaultIndexVersion},
            {keyPattern: {withV1: 1}, version: defaultIndexVersion},
            {keyPattern: {withV2: 1}, version: defaultIndexVersion},
        ];
    } else {
        expectedResults = [
            {keyPattern: {_id: 1}, version: 1},
            {keyPattern: {withoutAnyOptions: 1}, version: defaultIndexVersion},
            {keyPattern: {withV1: 1}, version: 1},
            {keyPattern: {withV2: 1}, version: 2},
        ];
    }

    expectedResults.forEach(function (expected) {
        let allIndexes = collToVerify.getIndexes();
        let spec = IndexCatalogHelpers.findByKeyPattern(allIndexes, expected.keyPattern);
        assert.neq(
            null,
            spec,
            "Index with key pattern " + tojson(expected.keyPattern) + " not found: " + tojson(allIndexes),
        );
        assert.eq(
            expected.version,
            spec.v,
            "Expected index to be rebuilt with " +
                (doesAutoUpgrade ? "the default" : "its original") +
                " version: " +
                tojson(spec),
        );
    });
}

// Test that the "reIndex" command upgrades all existing indexes to the latest version.
testIndexVersionAutoUpgrades(function (coll) {
    assert.commandWorked(coll.getDB().runCommand({reIndex: coll.getName()}));
    return coll;
}, true);

// Test that the "compact" command doesn't upgrade existing indexes to the latest version.
testIndexVersionAutoUpgrades(function (coll) {
    let res = coll.getDB().runCommand({compact: coll.getName()});
    if (res.ok === 0) {
        // Ephemeral storage engines don't support the "compact" command. The existing indexes
        // should remain unchanged. Also, it's possible for compact to be interrupted due to cache
        // pressure or concurrent compact calls.
        assert.commandFailedWithCode(res, [ErrorCodes.CommandNotSupported, ErrorCodes.Interrupted], tojson(res));
    }
    return coll;
}, false);

MongoRunner.stopMongod(conn);
