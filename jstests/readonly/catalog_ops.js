import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {runReadOnlyTest} from "jstests/readonly/lib/read_only_test.js";

runReadOnlyTest(
    (function () {
        return {
            name: "catalog_ops",

            collectionNames: ["foo", "bar", "baz", "garply"],
            tsCollectionName: "tsColl",
            indexSpecs: [{a: 1}, {a: 1, b: -1}, {a: 1, b: 1, c: -1}],

            load: function (writableCollection) {
                // Catalog guarantees are neccessarily weaker in sharded systems since mongos is not
                // read-only aware.
                if (TestData.fixture === "sharded") return;

                let db = writableCollection.getDB();

                // Create some collections so we can verify that listCollections works in read-only
                // mode.
                for (let collectionName of this.collectionNames) {
                    assert.commandWorked(db.runCommand({create: collectionName}));
                }

                // Create also a timeseries collection to validate that listCollections still works.
                assert.commandWorked(
                    db.createCollection(this.tsCollectionName, {timeseries: {timeField: "time", metaField: "meta"}}),
                );

                this.collectionNames.push(this.tsCollectionName, getTimeseriesCollForDDLOps(db, this.tsCollectionName));

                // Create some indexes so we can verify that listIndexes works in read-only mode.
                for (let indexSpec of this.indexSpecs) {
                    assert.commandWorked(writableCollection.createIndex(indexSpec));
                }
            },
            exec: function (readableCollection) {
                // Catalog guarantees are neccessarily weaker in sharded systems since mongos is not
                // read-only aware.
                if (TestData.fixture === "sharded") return;

                // Check that we can read our collections out.
                const db = readableCollection.getDB();

                // Check that listCollections is working and prints collection information with readOnly
                // true.
                const collections = db.getCollectionInfos();

                this.collectionNames.forEach((expectedCollectionName) => {
                    const outputColl = collections.find((coll) => coll.name === expectedCollectionName);
                    assert(
                        outputColl,
                        "expected collection '" +
                            expectedCollectionName +
                            "' to be readOnly, but according to listCollections output it isn't. " +
                            tojson(collections),
                    );
                    assert(
                        outputColl.info.readOnly,
                        "Collection '" +
                            expectedCollectionName +
                            "' not found in the output of listCollections, which was " +
                            tojson(collections),
                    );
                });

                // Check that create fails.
                assert.commandFailedWithCode(db.runCommand({create: "quux"}), [ErrorCodes.IllegalOperation]);

                // Check that implicit create fails.
                assert.commandFailedWithCode(db.quux.insert({a: 1}), [ErrorCodes.IllegalOperation]);
                assert.commandFailedWithCode(db.foo.update({a: 1}, {a: 1}, {upsert: true}), [
                    ErrorCodes.IllegalOperation,
                ]);

                // Check that drop fails.
                assert.commandFailedWithCode(db.runCommand({drop: "foo"}), [ErrorCodes.IllegalOperation]);

                // Check that dropDatabase fails.
                assert.commandFailedWithCode(db.runCommand({dropDatabase: 1}), [ErrorCodes.IllegalOperation]);

                // Check that renameCollection fails
                assert.commandFailedWithCode(
                    db.adminCommand({renameCollection: db.getName() + ".foo", to: db.getName() + ".foo2"}),
                    [ErrorCodes.IllegalOperation],
                );

                // Check that we can read our indexes out.
                let indexes = readableCollection.getIndexes();
                let actualIndexes = indexes.map((fullSpec) => {
                    return fullSpec.key;
                });
                let expectedIndexes = [{_id: 1}].concat(this.indexSpecs);

                assert.docEq(expectedIndexes, actualIndexes);

                // Check that createIndexes fails.
                assert.commandFailedWithCode(
                    db.runCommand({createIndexes: this.name, indexes: [{key: {d: 1}, name: "foo"}]}),
                    [ErrorCodes.IllegalOperation],
                );

                // Check that dropIndexes fails.
                assert.commandFailedWithCode(db.runCommand({dropIndexes: this.name, index: {a: 1}}), [
                    ErrorCodes.IllegalOperation,
                ]);
            },
        };
    })(),
);
