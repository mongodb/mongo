load("jstests/readonly/lib/read_only_test.js");

runReadOnlyTest(function() {
    'use strict';
    return {
        name: 'catalog_ops',

        collectionNames: ["foo", "bar", "baz", "garply"],
        indexSpecs: [{a: 1}, {a: 1, b: -1}, {a: 1, b: 1, c: -1}],

        load: function(writableCollection) {

            // Catalog guarantees are neccessarily weaker in sharded systems since mongos is not
            // read-only aware.
            if (TestData.fixture === "sharded")
                return;

            var db = writableCollection.getDB();

            // Create some collections so we can verify that listCollections works in read-only
            // mode.
            for (var collectionName of this.collectionNames) {
                assert.commandWorked(db.runCommand({create: collectionName}));
            }

            // Create some indexes so we can verify that listIndexes works in read-only mode.
            for (var indexSpec of this.indexSpecs) {
                assert.commandWorked(writableCollection.createIndex(indexSpec));
            }
        },
        exec: function(readableCollection) {

            // Catalog guarantees are neccessarily weaker in sharded systems since mongos is not
            // read-only aware.
            if (TestData.fixture === "sharded")
                return;

            // Check that we can read our collections out.
            var db = readableCollection.getDB();
            var collections = db.getCollectionNames();  // runs listCollections internally.
            for (var collectionName of this.collectionNames) {
                assert.contains(collectionName,
                                collections,
                                "expected to have a collection '" + collectionName +
                                    "' in the output of listCollections, which was " +
                                    tojson(collections));
            }
            assert.gte(collections.length, this.collectionNames.length);

            // Check that create fails.
            assert.commandFailed(db.runCommand({create: "quux"}));

            // Check that drop fails.
            assert.commandFailed(db.runCommand({drop: "foo"}));

            // Check that dropDatabase fails.
            assert.commandFailed(db.runCommand({dropDatabase: 1}));

            // Check that we can read our indexes out.
            var indexes = readableCollection.getIndexes();
            var actualIndexes = indexes.map((fullSpec) => {
                return fullSpec.key;
            });
            var expectedIndexes = Array.concat([{_id: 1}], this.indexSpecs);

            assert.docEq(actualIndexes, expectedIndexes);

            // Check that createIndexes fails.
            assert.commandFailed(
                db.runCommand({createIndexes: this.name, indexes: [{key: {d: 1}, name: "foo"}]}));
        }
    };
}());
