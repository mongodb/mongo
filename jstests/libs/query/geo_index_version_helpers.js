/**
 * Helper functions for handling 2dsphere index version compatibility in FCV upgrade/downgrade
 * test suites.
 *
 * When running tests during FCV upgrade/downgrade, we need to pin 2dsphere indexes to version 3
 * to avoid compatibility issues. Version 4 2dsphere indexes cannot be downgraded and must be
 * dropped before downgrading FCV, which can cause test failures.
 */

/**
 * Adds 2dsphereIndexVersion: 3 to index options if running in an FCV upgrade/downgrade suite.
 *
 * This helper should be used when creating 2dsphere indexes in tests that run during FCV
 * transitions to ensure the index can be maintained across FCV changes without requiring
 * dropping and recreation.
 *
 * TODO SERVER-118561 Remove this and use the default server-selected version when 9.0 is last LTS.
 *
 * @param {Object} options - Optional index creation options object. Will be created if not provided.
 * @returns {Object} The options object with 2dsphereIndexVersion set to 3 if needed.
 *
 * @example
 * // Simple 2dsphere index
 * coll.createIndex({geo: "2dsphere"}, add2dsphereVersionIfNeeded());
 *
 * @example
 * // With additional options
 * coll.createIndex({geo: "2dsphere"}, add2dsphereVersionIfNeeded({name: "my_geo_index"}));
 *
 * @example
 * // Compound index
 * coll.createIndex({geo: "2dsphere", category: 1}, add2dsphereVersionIfNeeded());
 */
export function add2dsphereVersionIfNeeded(options = {}) {
    if (TestData.isRunningFCVUpgradeDowngradeSuite) {
        // Pin the index version to v3 when upgrading/downgrading FCV during the test run,
        // such that we don't need to drop v4 indexes to downgrade the FCV.
        options["2dsphereIndexVersion"] = 3;
    }
    return options;
}

/**
 * Checks if the given index specification contains a 2dsphere index.
 *
 * @param {Object} spec - The index specification to check.
 * @returns {boolean} True if the spec contains a 2dsphere index, false otherwise.
 *
 * @example
 * has2dsphereIndex({geo: "2dsphere"}) // returns true
 * has2dsphereIndex({geo: "2dsphere", name: 1}) // returns true
 * has2dsphereIndex({name: 1}) // returns false
 */
export function has2dsphereIndex(spec) {
    for (const key in spec) {
        if (spec[key] === "2dsphere") {
            return true;
        }
    }
    return false;
}

/**
 * Conditionally adds 2dsphereIndexVersion: 3 based on whether the index spec contains a 2dsphere
 * index and whether running in FCV upgrade/downgrade suite.
 *
 * This is useful when you have dynamic index specs that may or may not include 2dsphere indexes.
 *
 * TODO SERVER-118561 Remove this and use the default server-selected version when 9.0 is last LTS.
 *
 * @param {Object} spec - The index specification.
 * @param {Object} options - Optional index creation options object.
 * @returns {Object} The options object with 2dsphereIndexVersion set to 3 if needed.
 *
 * @example
 * // Only sets version if spec contains 2dsphere
 * coll.createIndex(dynamicSpec, add2dsphereVersionIfNeededForSpec(dynamicSpec));
 */
export function add2dsphereVersionIfNeededForSpec(spec, options = {}) {
    if (has2dsphereIndex(spec) && TestData.isRunningFCVUpgradeDowngradeSuite) {
        options["2dsphereIndexVersion"] = 3;
    }
    return options;
}
