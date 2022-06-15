/**
 * Utilities for feature flags.
 */
var FeatureFlagUtil = class {
    /**
     * Returns true if feature flag is enabled, false otherwise.
     */
    static isEnabled(db, featureFlag) {
        return eval(
            `if (db["_mongo"] != undefined &&
                    db["_mongo"]["fullOptions"] != undefined &&
                    db["_mongo"]["fullOptions"]["pathOpts"] != undefined &&
                    db["_mongo"]["fullOptions"]["pathOpts"]["mongos"] != undefined) {
                throw new Error("Database must not be taken from mongos");
            }
            const admin = db.getSiblingDB("admin");
            const flagDoc = admin.runCommand({getParameter: 1, featureFlag${featureFlag}: 1});
            const fcvDoc = admin.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
            flagDoc.hasOwnProperty("featureFlag${featureFlag}") &&
                flagDoc.featureFlag${featureFlag}.value &&
                (!fcvDoc.hasOwnProperty("featureCompatibilityVersion") ||
                MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version,
                                            flagDoc.featureFlag${featureFlag}.version) >= 0);`
        );
    }
};
