"use strict";

/**
 * Helpers for filtering the index specifications returned by DBCollection.prototype.getIndexes().
 */
var GetIndexHelpers = (function() {

    /**
     * Returns the index specification with the name 'indexName' if it is present in the
     * 'indexSpecs' array, and returns null otherwise.
     */
    function getIndexSpecByName(indexSpecs, indexName) {
        if (typeof indexName !== "string") {
            throw new Error("'indexName' parameter must be a string, but got " + tojson(indexName));
        }

        const found = indexSpecs.filter(spec => spec.name === indexName);

        if (found.length > 1) {
            throw new Error("Found multiple indexes with name '" + indexName + "': " +
                            tojson(indexSpecs));
        }
        return (found.length === 1) ? found[0] : null;
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
        const foundByKeyPattern = indexSpecs.filter(spec => {
            return bsonWoCompare(spec.key, keyPattern) === 0;
        });

        if (!collationWasSpecified) {
            if (foundByKeyPattern.length > 1) {
                throw new Error("Found multiple indexes with key pattern " + tojson(keyPattern) +
                                " and 'collation' parameter was not specified: " +
                                tojson(indexSpecs));
            }
            return (foundByKeyPattern.length === 1) ? foundByKeyPattern[0] : null;
        }

        const foundByKeyPatternAndCollation = foundByKeyPattern.filter(spec => {
            if (collation.locale === "simple") {
                // The simple collation is not explicitly stored in the index catalog, so we expect
                // the "collation" field to be absent.
                return !spec.hasOwnProperty("collation");
            }
            return bsonWoCompare(spec.collation, collation) === 0;
        });

        if (foundByKeyPatternAndCollation.length > 1) {
            throw new Error("Found multiple indexes with key pattern" + tojson(keyPattern) +
                            " and collation " + tojson(collation) + ": " + tojson(indexSpecs));
        }
        return (foundByKeyPatternAndCollation.length === 1) ? foundByKeyPatternAndCollation[0]
                                                            : null;
    }

    return {
        findByName: getIndexSpecByName,
        findByKeyPattern: getIndexSpecByKeyPattern,
    };
})();
