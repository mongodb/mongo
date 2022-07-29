"use strict";

load("jstests/libs/fixture_helpers.js");

/**
 * Utilities for feature flags.
 */
var FeatureFlagUtil = class {
    /**
     * Returns true if feature flag is enabled, false otherwise.
     */
    static isEnabled(db, featureFlag, user) {
        // In order to get an accurate answer for whether a feature flag is enabled, we need to ask
        // a mongod. If db represents a connection to mongos, or some other configuration, we need
        // to obtain the correct connection to a mongod.
        let conn;
        const setConn = (db) => {
            if (FixtureHelpers.isMongos(db)) {
                const primaries = FixtureHelpers.getPrimaries(db);
                assert.gt(primaries.length, 0, "Expected at least one primary");
                conn = primaries[0];
            } else {
                conn = db;
            }
        };
        try {
            setConn(db);
        } catch (err) {
            // Some db-like objects (e.g. ShardingTest.shard0) aren't supported by FixtureHelpers,
            // but we can replace it with an object that should work and try again.
            setConn(db.getDB(db.defaultDB));
        }

        if (user) {
            conn.auth(user.username, user.password);
        }

        const fcvDoc = assert.commandWorked(
            conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));

        const fullFlagName = `featureFlag${featureFlag}`;
        const flagDoc = conn.adminCommand({getParameter: 1, [fullFlagName]: 1});
        if (!flagDoc.ok) {
            // Feature flag not found.
            assert.eq(flagDoc.errmsg, "no option found to get");
            return false;
        }

        return flagDoc.hasOwnProperty(fullFlagName) && flagDoc[fullFlagName].value &&
            (!fcvDoc.hasOwnProperty("featureCompatibilityVersion") ||
             MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version,
                                            flagDoc[fullFlagName].version) >= 0);
    }
};
