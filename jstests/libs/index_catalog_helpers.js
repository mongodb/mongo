import {isStableFCVSuite} from "jstests/libs/feature_compatibility_version.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

/**
 * Helper functions that help test things to do with the index catalog.
 */
export var IndexCatalogHelpers = (function () {
    /**
     * Returns the index specification with the name 'indexName' if it is present in the
     * 'indexSpecs' array, and returns null otherwise.
     */
    function getIndexSpecByName(indexSpecs, indexName) {
        if (typeof indexName !== "string") {
            throw new Error("'indexName' parameter must be a string, but got " + tojson(indexName));
        }

        const found = indexSpecs.filter((spec) => spec.name === indexName);

        if (found.length > 1) {
            throw new Error("Found multiple indexes with name '" + indexName + "': " + tojson(indexSpecs));
        }
        return found.length === 1 ? found[0] : null;
    }

    /**
     * Returns the index specification with the key pattern 'keyPattern' and the collation
     * 'collation' if it is present in the 'indexSpecs' array, and returns null otherwise.
     *
     * The 'collation' parameter is optional and is only required to be specified when multiple
     * indexes with the same key pattern exist.
     */
    function getIndexSpecByKeyPattern(indexSpecs, keyPattern, collation) {
        const collationWasSpecified = arguments.length >= 3;
        const foundByKeyPattern = indexSpecs.filter((spec) => {
            return bsonWoCompare(spec.key, keyPattern) === 0;
        });

        if (!collationWasSpecified) {
            if (foundByKeyPattern.length > 1) {
                throw new Error(
                    "Found multiple indexes with key pattern " +
                        tojson(keyPattern) +
                        " and 'collation' parameter was not specified: " +
                        tojson(indexSpecs),
                );
            }
            return foundByKeyPattern.length === 1 ? foundByKeyPattern[0] : null;
        }

        const foundByKeyPatternAndCollation = foundByKeyPattern.filter((spec) => {
            if (collation.locale === "simple") {
                // The simple collation is not returned by listIndexes on older versions.
                // On newer versions, the simple collation is always returned by listIndexes.
                // TODO (SERVER-122417) Remove this workaround once v9.0 branches out.
                return !spec.hasOwnProperty("collation") || spec.collation.locale === "simple";
            }
            return bsonWoCompare(spec.collation, collation) === 0;
        });

        if (foundByKeyPatternAndCollation.length > 1) {
            throw new Error(
                "Found multiple indexes with key pattern" +
                    tojson(keyPattern) +
                    " and collation " +
                    tojson(collation) +
                    ": " +
                    tojson(indexSpecs),
            );
        }
        return foundByKeyPatternAndCollation.length === 1 ? foundByKeyPatternAndCollation[0] : null;
    }

    function createSingleIndex(coll, key, parameters) {
        return coll
            .getDB()
            .runCommand({createIndexes: coll.getName(), indexes: [Object.assign({key: key}, parameters)]});
    }

    function createIndexAndVerifyWithDrop(coll, key, parameters) {
        coll.dropIndexes();
        assert.commandWorked(createSingleIndex(coll, key, parameters));
        assert.neq(
            null,
            getIndexSpecByName(coll.getIndexes(), parameters.name),
            () => `Could not find index with name ${parameters.name}: ${tojson(coll.getIndexes())}`,
        );
    }

    /**
     * Converts the index specs returned by listIndexes to the storage index format by stripping out
     * the 'collation' field if it is a simple collation.
     *
     * As of SERVER-89953, every index spec returned by listIndexes includes a 'collation' field,
     * even when it is just the default "simple" collation. However, the format stored in the catalog
     * omits the 'collation' field for simple collations.
     *
     * TODO (SERVER-119573): Remove this utility once listIndexes output is consistent with storage.
     */
    function convertListIndexesResponseToStorageIndexFormat(indexes) {
        return indexes.map((index) => {
            if (index.collation && index.collation.locale === "simple") {
                const {collation, ...rest} = index;
                return rest;
            }
            return index;
        });
    }

    /**
     * Returns true if listIndexes always includes the simple collation in index specs.
     *
     * In FCV upgrade/downgrade suites, listIndexes may not include the simple collation if the feature
     * flag was not enabled in the lastLTS FCV. This function handles both stable and upgrade/downgrade
     * suite contexts.
     *
     * TODO (SERVER-122417): Remove this function once v9.0 branches out.
     */
    function listIndexesIncludesSimpleCollation(db) {
        if (isStableFCVSuite()) {
            return FeatureFlagUtil.isPresentAndEnabled(db, "ListIndexesAlwaysIncludesSimpleCollation");
        }

        // In FCV upgrade/downgrade suite, the flag is reliably enabled only if it has been enabled since
        // lastLTS (e.g., flag released in FCV 9.0, running binary 9.1 with FCV 9.0 - 9.1
        // upgrade/downgrade suite --> always includes simple collation).
        const flagDoc = FeatureFlagUtil.getFeatureFlagDoc(db.getMongo(), "ListIndexesAlwaysIncludesSimpleCollation");
        return flagDoc && flagDoc.value && MongoRunner.compareBinVersions(lastLTSFCV, flagDoc.version) >= 0;
    }

    /**
     * Ensures an index spec includes {locale: "simple"} collation if missing from listIndexes output.
     *
     * listIndexes may omit the explicit simple collation in downgrade/upgrade FCV suites where the
     * relevant feature flag was not enabled in the lastLTS FCV. This helper adds it if needed to
     * standardize index specs for comparison and correctness across test environments.
     *
     * TODO (SERVER-122417): Remove this function once v9.0 branches out.
     */
    function addSimpleCollationToIndexIfMissing(db, index) {
        if (listIndexesIncludesSimpleCollation(db)) {
            return index;
        }

        let indexWithSimpleCollation = {...index};
        if (!index.collation) {
            indexWithSimpleCollation.collation = {locale: "simple"};
        }
        if (index.originalSpec && !index.originalSpec.collation) {
            indexWithSimpleCollation.originalSpec.collation = {locale: "simple"};
        }
        return indexWithSimpleCollation;
    }

    /**
     * Ensures the given list of index specs includes {locale: "simple"} collation if missing from
     * listIndexes output.
     *
     * listIndexes may omit the explicit simple collation in downgrade/upgrade FCV suites where the
     * relevant feature flag was not enabled in the lastLTS FCV. This helper adds it if needed to
     * standardize index specs for comparison and correctness across test environments.
     *
     * TODO (SERVER-122417): Remove this function once v9.0 branches out.
     */
    function addSimpleCollationToIndexesIfMissing(db, indexes) {
        if (listIndexesIncludesSimpleCollation(db)) {
            return indexes;
        }
        return indexes.map((index) => addSimpleCollationToIndexIfMissing(db, index));
    }

    return {
        findByName: getIndexSpecByName,
        findByKeyPattern: getIndexSpecByKeyPattern,
        createSingleIndex: createSingleIndex,
        createIndexAndVerifyWithDrop: createIndexAndVerifyWithDrop,
        // TODO (SERVER-119573): Remove this utility once listIndexes output is consistent with storage.
        convertListIndexesResponseToStorageIndexFormat: convertListIndexesResponseToStorageIndexFormat,
        // TODO (SERVER-122417): Remove these utilities once v9.0 branches out.
        addSimpleCollationToIndexIfMissing: addSimpleCollationToIndexIfMissing,
        addSimpleCollationToIndexesIfMissing: addSimpleCollationToIndexesIfMissing,
    };
})();
