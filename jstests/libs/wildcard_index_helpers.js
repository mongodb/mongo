/**
 * Common utility functions for testing functionality of Wildcard Indexes.
 */

"use strict";

const WildcardIndexHelpers = (function() {
    load("jstests/libs/analyze_plan.js");

    /**
     * Asserts that the given explain contains the given expectedIndexName in the winningPlan.
     */
    function assertExpectedIndexIsUsed(explain, expectedIndexName) {
        const winningPlan = getWinningPlan(explain.queryPlanner);
        const planStages = getPlanStages(winningPlan, 'IXSCAN');

        assert.neq(0, planStages.length, explain);

        for (const stage of planStages) {
            assert(stage.hasOwnProperty('indexName'), stage);
            assert.eq(stage.indexName, expectedIndexName, stage);
        }
    }

    /**
     * Asserts that the given explain does not contain the given expectedIndexName in the
     * winningPlan.
     */
    function assertExpectedIndexIsNotUsed(explain, expectedIndexName) {
        const winningPlan = getWinningPlan(explain.queryPlanner);
        const planStages = getPlanStages(winningPlan, 'IXSCAN');

        // It is fine if no IXSCAN's were found for it is guarantee the index was not used.
        for (const stage of planStages) {
            assert(stage.hasOwnProperty('indexName'), stage);
            assert.neq(stage.indexName, expectedIndexName, stage);
        }
    }

    /**
     * Returns index name for the given keyPattern.
     */
    function getIndexName(coll, keyPattern) {
        const indexes = coll.getIndexes();
        const index = indexes.find(index => bsonWoCompare(index.key, keyPattern) == 0);
        if (index !== undefined) {
            return index.name;
        }
        return null;
    }

    /**
     * Returns index object using the given indexSpec of format {keyPattern: {...}}.
     */
    function findIndex(coll, indexSpec) {
        const indexes = coll.getIndexes();

        for (const index of indexes) {
            if (bsonWoCompare(index.key, indexSpec.keyPattern) == 0) {
                return index;
            }
        }

        return null;
    }

    /**
     * Creates index using the given indexSpec of format {keyPattern: {...},
     * wildcardProjection: {...}, hidden: bool}. Only 'keyPattern' filed is required. Assigns the
     * created index name in the field 'indexName' of the given indexSpec.
     */
    function createIndex(coll, indexSpec) {
        const options = {};
        if (indexSpec.wildcardProjection) {
            options["wildcardProjection"] = indexSpec.wildcardProjection;
        }
        if (indexSpec.hidden) {
            options["hidden"] = true;
        }

        assert.commandWorked(coll.createIndex(indexSpec.keyPattern, options));
        indexSpec.indexName = getIndexName(coll, indexSpec.keyPattern);
        assert.neq(null, indexSpec.indexName);
    }

    /**
     * Validates whether the index hidden or not using the given index specification 'indexSpec' of
     * format {keyPattern:{}, hidden: bool}. Only keyPattern is required.
     */
    function validateIndexVisibility(coll, indexSpec) {
        const index = findIndex(coll, indexSpec);
        assert.neq(null, index);

        if (indexSpec.hidden) {
            assert.eq(true, index.hidden);
        } else {
            assert.neq(true, index.hidden);
        }
    }

    return {
        assertExpectedIndexIsUsed,
        assertExpectedIndexIsNotUsed,
        findIndex,
        createIndex,
        validateIndexVisibility,
    };
})();
