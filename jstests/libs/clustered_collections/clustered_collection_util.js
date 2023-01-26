/**
 * Utilities for testing clustered collections.
 */

load("jstests/libs/analyze_plan.js");
load("jstests/libs/collection_drop_recreate.js");

var ClusteredCollectionUtil = class {
    static areAllCollectionsClustered(conn) {
        const res =
            conn.adminCommand({getParameter: 1, "failpoint.clusterAllCollectionsByDefault": 1});
        if (res.ok)
            return res["failpoint.clusterAllCollectionsByDefault"].mode;
        else
            return false;
    }

    static isArbitraryKeySupportEnabled(conn) {
        const arbitraryKeySupportEnabled =
            assert
                .commandWorked(
                    conn.adminCommand({getParameter: 1, supportArbitraryClusterKeyIndex: 1}))
                .supportArbitraryClusterKeyIndex.value;
        return arbitraryKeySupportEnabled;
    }

    // Returns a copy of the 'createOptions' used to create the clustered collection with default
    // values for fields absent in the user provided 'createOptions'.
    static constructFullCreateOptions(createOptions) {
        const fullCreateOptions = Object.extend({}, createOptions, /* deep copy */ true);

        // If the createOptions don't specify the name, expect the default.
        if (!createOptions.clusteredIndex.name) {
            const clusterKey = Object.keys(createOptions.clusteredIndex.key)[0];
            if (clusterKey == "_id") {
                fullCreateOptions.clusteredIndex.name = "_id_";
            } else {
                fullCreateOptions.clusteredIndex.name = clusterKey + "_1";
            }
        }

        // If the createOptions don't specify 'v', expect the default.
        if (!createOptions.clusteredIndex.v) {
            fullCreateOptions.clusteredIndex.v = 2;
        }

        return fullCreateOptions;
    }

    static validateListCollectionsNotClustered(db, collName) {
        const listColls =
            assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
        const listCollsOptions = listColls.cursor.firstBatch[0].options;
        assert.eq(
            listCollsOptions.clusteredIndex, undefined, "Expected clusteredIndex to be undefined");
    }

    static validateListIndexesNonClustered(db, collName) {
        const listIndexes = assert.commandWorked(db[collName].runCommand("listIndexes"));
        assert.eq(listIndexes.cursor.firstBatch[0].clustered || false,
                  false,
                  "Index had clustering in it when it shouldn't");
    }

    // Provided the createOptions used to create the collection, validates the output from
    // listCollections contains the correct information about the clusteredIndex.
    static validateListCollections(db, collName, createOptions) {
        const fullCreateOptions = ClusteredCollectionUtil.constructFullCreateOptions(createOptions);
        const listColls =
            assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
        const listCollsOptions = listColls.cursor.firstBatch[0].options;
        assert(listCollsOptions.clusteredIndex);
        assert.docEq(fullCreateOptions.clusteredIndex, listCollsOptions.clusteredIndex);
    }

    // The clusteredIndex should appear in listIndexes with additional "clustered" field.
    static validateListIndexes(db, collName, createOptions) {
        const fullCreateOptions = ClusteredCollectionUtil.constructFullCreateOptions(createOptions);
        const listIndexes = assert.commandWorked(db[collName].runCommand("listIndexes"));
        const expectedListIndexesOutput =
            Object.extend({clustered: true}, fullCreateOptions.clusteredIndex);
        assert.docEq(expectedListIndexesOutput, listIndexes.cursor.firstBatch[0]);
    }

    static testBasicClusteredCollection(db, collName, clusterKey) {
        const lengths = [100, 1024, 1024 * 1024, 3 * 1024 * 1024];
        const coll = db[collName];
        const clusterKeyString = new String(clusterKey);

        assert.commandWorked(db.createCollection(
            collName, {clusteredIndex: {key: {[clusterKey]: 1}, unique: true}}));

        // Expect that duplicates are rejected.
        for (let len of lengths) {
            let id = 'x'.repeat(len);
            assert.commandWorked(coll.insert({[clusterKey]: id}));
            assert.commandFailedWithCode(coll.insert({[clusterKey]: id}), ErrorCodes.DuplicateKey);
            assert.eq(1, coll.find({[clusterKey]: id}).itcount());
        }

        // Updates should work.
        for (let len of lengths) {
            let id = 'x'.repeat(len);

            // Validate the below for _id-clustered collection only given replacement updates only
            // preserve cluster key '_id'.
            if (clusterKey == "_id") {
                assert.commandWorked(coll.update({[clusterKey]: id}, {a: len}));

                assert.eq(1, coll.find({[clusterKey]: id}).itcount());
                assert.eq(len, coll.findOne({[clusterKey]: id})['a']);
            }
        }

        // This section is based on jstests/core/timeseries/clustered_index_crud.js with
        // specific additions for general-purpose (non-timeseries) clustered collections
        assert.commandWorked(coll.insert({[clusterKey]: 0, a: 1}));
        assert.commandWorked(coll.insert({[clusterKey]: 1, a: 1}));
        assert.eq(1, coll.find({[clusterKey]: 0}).itcount());
        assert.commandWorked(coll.insert({[clusterKey]: "", a: 2}));
        assert.eq(1, coll.find({[clusterKey]: ""}).itcount());
        assert.commandWorked(coll.insert({[clusterKey]: NumberLong("9223372036854775807"), a: 3}));
        assert.eq(1, coll.find({[clusterKey]: NumberLong("9223372036854775807")}).itcount());
        assert.commandWorked(coll.insert({[clusterKey]: {a: 1, b: 1}, a: 4}));
        assert.eq(1, coll.find({[clusterKey]: {a: 1, b: 1}}).itcount());
        assert.commandWorked(coll.insert({[clusterKey]: {a: {b: 1}, c: 1}, a: 5}));
        assert.commandWorked(coll.insert({[clusterKey]: -1, a: 6}));
        assert.eq(1, coll.find({[clusterKey]: -1}).itcount());
        assert.commandWorked(coll.insert({[clusterKey]: "123456789012", a: 7}));
        assert.eq(1, coll.find({[clusterKey]: "123456789012"}).itcount());
        if (clusterKey == "_id") {
            assert.commandWorked(coll.insert({a: 8}));
        } else {
            // Missing required cluster key field.
            assert.commandFailedWithCode(coll.insert({a: 8}), 2);
            assert.commandWorked(coll.insert({[clusterKey]: "withFieldA", a: 8}));
        }
        assert.eq(1, coll.find({a: 8}).itcount());
        assert.commandWorked(coll.insert({[clusterKey]: null, a: 9}));
        assert.eq(1, coll.find({[clusterKey]: null}).itcount());
        assert.commandWorked(coll.insert({[clusterKey]: 'x'.repeat(99), a: 10}));

        if (clusterKey == "_id") {
            assert.commandWorked(coll.insert({}));
        } else {
            // Missing required ts field.
            assert.commandFailedWithCode(coll.insert({}), 2);
            assert.commandWorked(coll.insert({[clusterKey]: 'missingFieldA'}));
        }
        // Can build a secondary index with a 3MB RecordId doc.
        assert.commandWorked(coll.createIndex({a: 1}));
        // Can drop the secondary index
        assert.commandWorked(coll.dropIndex({a: 1}));

        // This key is too large.
        assert.commandFailedWithCode(
            coll.insert({[clusterKey]: 'x'.repeat(9 * 1024 * 1024), a: 11}), 5894900);

        // Look up using the secondary index on {a: 1}
        assert.commandWorked(coll.createIndex({a: 1}));

        if (clusterKey == "_id") {
            // Replacement updates only preserve the '_id' cluster key.
            assert.eq(1, coll.find({a: null}).itcount());
        } else {
            assert.eq(5, coll.find({a: null}).itcount());
        }
        assert.eq(0, coll.find({a: 0}).itcount());
        assert.eq(2, coll.find({a: 1}).itcount());
        assert.eq(1, coll.find({a: 2}).itcount());
        assert.eq(1, coll.find({a: 8}).itcount());
        assert.eq(1, coll.find({a: 9}).itcount());
        assert.eq(null, coll.findOne({a: 9})[clusterKeyString]);
        assert.eq(1, coll.find({a: 10}).itcount());
        assert.eq(99, coll.findOne({a: 10})[clusterKeyString].length);

        if (clusterKey == "_id") {
            // Replacement updates only preserve the '_id' cluster key.
            for (let len of lengths) {
                // Secondary index lookups for documents with large RecordId's.
                assert.eq(1, coll.find({a: len}).itcount());
                assert.eq(len, coll.findOne({a: len})[clusterKeyString].length);
            }
        }

        // No support for numeric type differentiation.
        assert.commandWorked(coll.insert({[clusterKey]: 42.0}));
        assert.commandFailedWithCode(coll.insert({[clusterKey]: 42}), ErrorCodes.DuplicateKey);
        assert.commandFailedWithCode(coll.insert({[clusterKey]: NumberLong("42")}),
                                     ErrorCodes.DuplicateKey);
        assert.eq(1, coll.find({[clusterKey]: 42.0}).itcount());
        assert.eq(1, coll.find({[clusterKey]: 42}).itcount());
        assert.eq(1, coll.find({[clusterKey]: NumberLong("42")}).itcount());
        coll.drop();
    }
};
