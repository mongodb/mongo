import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

export var IndexUtils = (function () {
    /**
     * Verifies that the indexes on 'coll' match those in 'expectedIndexes'.
     *
     * This function takes into account indexes that may have been created implicitly. For suites
     * that implicitly shard collections, indexes such as the shard key or _id may exist. Similarly,
     * for suites that implicitly create a wildcard index, those are also considered.
     */
    function assertIndexes(coll, expectedIndexes, msg) {
        const actualIndexes = coll.getIndexes().map((spec) => spec.key);
        assertIndexesMatch(coll, expectedIndexes, actualIndexes, msg);
    }

    /**
     * Verifies that the 'actualIndexes' match those in 'expectedIndexes' for the given collection.
     *
     * This function takes into account indexes that may have been created implicitly. For suites
     * that implicitly shard collections, indexes such as the shard key or _id may exist. Similarly,
     * for suites that implicitly create a wildcard index, those are also considered.
     */
    function assertIndexesMatch(coll, expectedIndexes, actualIndexes, msg) {
        assert(actualIndexes);

        const addIfNotExists = (arr, value) => {
            if (!arr.some((v) => v === value || bsonUnorderedFieldsCompare(v, value) === 0)) {
                arr.push(value);
            }
        };

        if (FixtureHelpers.isSharded(coll)) {
            // If the collection has been dropped, it will be automatically recreated on 'sharded'
            // suites. Hence, the index {_id: 1} will exist.
            //
            // Note that this doesn't affect unsharded tracked collections because the drop
            // operation doesn't recreate the collection on suites that implicitly create
            // unsplittable collections.
            addIfNotExists(expectedIndexes, {_id: 1});

            // If the shard key index exists, add it to the expected indexes.
            const shardKey = coll.getShardKey();
            if (coll.getIndexByKey(shardKey)) {
                addIfNotExists(expectedIndexes, shardKey);
            }
        }

        if (TestData.implicitWildcardIndexesEnabled) {
            actualIndexes.forEach((index) => {
                if (Object.keys(index).some((key) => key.endsWith("$**"))) {
                    addIfNotExists(expectedIndexes, index);
                }
            });
        }

        assert.sameMembers(expectedIndexes, actualIndexes, msg);
    }

    /**
     * Checks whether the specified index exists.
     *
     * If `options` are provided, only the specified fields will be verified.
     * To check that a field does not exist, you can use the syntax: { field: undefined }.
     */
    function indexExists(coll, indexKey, options = undefined) {
        return coll
            .getIndexes()
            .some(
                (index) =>
                    bsonWoCompare(indexKey, index.key) === 0 &&
                    (!options ||
                        Object.keys(options).every(
                            (optionKey) => bsonWoCompare(options[optionKey], index[optionKey]) === 0,
                        )),
            );
    }

    return {
        assertIndexes: assertIndexes,
        assertIndexesMatch: assertIndexesMatch,
        indexExists: indexExists,
    };
})();
