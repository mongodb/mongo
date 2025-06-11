import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

/**
 * Utilities for feature flags.
 */
export var FeatureFlagUtil = (function() {
    // A JS attempt at an enum.
    const FlagStatus = {
        kEnabled: 'kEnabled',
        kDisabled: 'kDisabled',
        kNotFound: 'kNotFound',
    };

    function _getConnectionToMongod(db) {
        // If db represents a connection to mongos, or some other configuration, we need
        // to obtain the correct connection to a mongod.
        const getMongodConn = (db) => {
            if (!FixtureHelpers.isMongos(db)) {
                return db;
            }

            // For sharded cluster get a connection to the config server through a Mongo
            // object. We may fail to connect if we are in a stepdown/terminate passthrough, so
            // retry on retryable errors. After the connection is established, runCommand overrides
            // should guarantee that subsequent operations on the connection are retried in the
            // event of network errors in suites where that possibility exists.
            return retryOnRetryableError(() => {
                return new Mongo(FixtureHelpers.getConfigServerConnString(db),
                                 undefined /*encryptedDBClientCallback */,
                                 {gRPC: false});
            });
        };
        try {
            return getMongodConn(db);
        } catch (err) {
            // Some db-like objects (e.g. ShardingTest.shard0) aren't supported by FixtureHelpers,
            // but we can replace it with an object that should work and try again.
            if (typeof db.getDB === typeof Function) {
                return getMongodConn(db.getDB(db.defaultDB));
            } else {
                // Some db-like objects (e.g ShardedClusterFixture) have a getSiblingDB method
                // instead of getDB, use that here to avoid an undefined error.
                return getMongodConn(db.getSiblingDB(db.getMongo().defaultDB));
            }
        }
    }

    function _getAuthenticatedConnectionToMongod(db, user) {
        let mongodConn = _getConnectionToMongod(db);
        if (user) {
            mongodConn.auth(user.username, user.password);
        }
        return mongodConn;
    }

    function _getFullFeatureFlagName(featureFlagName) {
        if (!featureFlagName.startsWith("featureFlag")) {
            return `featureFlag${featureFlagName}`;
        }
        return featureFlagName;
    }

    function _getFeatureFlagDoc(conn, featureFlagName) {
        const fullFlagName = _getFullFeatureFlagName(featureFlagName);
        const parameterDoc = conn.adminCommand({getParameter: 1, [fullFlagName]: 1});
        if (!parameterDoc.ok || !parameterDoc.hasOwnProperty(fullFlagName)) {
            // Feature flag not found.
            if (!parameterDoc.ok) {
                assert.eq(parameterDoc.errmsg, "no option found to get");
            }
            return undefined;
        }
        return parameterDoc[fullFlagName];
    }

    function _getStatusLegacy(conn, ignoreFCV, flagDoc) {
        const fcvDoc = assert.commandWorked(
            conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));
        assert(fcvDoc.hasOwnProperty("featureCompatibilityVersion"), fcvDoc);

        const flagIsEnabled = flagDoc.value;
        const flagVersionIsValid =
            MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version,
                                           flagDoc.version) >= 0;

        const flagShouldBeFCVGated = flagDoc.fcv_gated;

        if (flagIsEnabled && (!flagShouldBeFCVGated || ignoreFCV || flagVersionIsValid)) {
            return FlagStatus.kEnabled;
        }
        return FlagStatus.kDisabled;
    }

    function _getStatus(ignoreFCV, flagDoc) {
        assert(flagDoc.hasOwnProperty("currentlyEnabled"));

        if (flagDoc.fcv_gated && ignoreFCV) {
            return flagDoc.value ? FlagStatus.kEnabled : FlagStatus.kDisabled;
        } else {
            return flagDoc.currentlyEnabled ? FlagStatus.kEnabled : FlagStatus.kDisabled;
        }
    }

    function getFeatureFlagDoc(db, flagName) {
        const conn = _getAuthenticatedConnectionToMongod(db);
        return _getFeatureFlagDoc(conn, flagName);
    }

    function getFeatureFlagDocStatus(db, flagDoc, ignoreFCV) {
        // TODO (SERVER-102609): Remove _getStatusLegacy() once v9.0 becomes last-LTS.
        return flagDoc.hasOwnProperty("currentlyEnabled")
            ? _getStatus(ignoreFCV, flagDoc)
            : _getStatusLegacy(_getAuthenticatedConnectionToMongod(db), ignoreFCV, flagDoc);
    }

    /**
     * @param 'featureFlag' - the name of the flag you want to check, but *without* the
     *     'featureFlag' prefix. For example, just "Toaster" instead of "featureFlagToaster."
     *
     * @param 'ignoreFcv' - If true, return whether or not the given feature flag is enabled,
     *     regardless of the current FCV version. This is used when a feature flag needs to be
     *     enabled in downgraded FCV versions. If 'ignoreFCV' is false or null, we only return true
     *     if the flag is enabled and this FCV version is greater or equal to the required version
     *     for the flag.
     *
     * @returns one of the 'FlagStatus' values indicating whether the flag is enabled, disabled, or
     *     not found. A flag may be not found because it was recently deleted or because the test is
     *     running on an older mongod version for example.
     */
    function getStatus(db, featureFlag, user, ignoreFCV) {
        // In order to get an accurate answer for whether a feature flag is enabled, we need to ask
        // a mongod.
        const conn = _getAuthenticatedConnectionToMongod(db);
        const flagDoc = _getFeatureFlagDoc(conn, featureFlag);
        return flagDoc ? getFeatureFlagDocStatus(db, flagDoc, ignoreFCV) : FlagStatus.kNotFound;
    }

    /**
     * @param 'featureFlag' - the name of the flag you want to check, but *without* the
     *     'featureFlag' prefix. For example, just "Toaster" instead of "featureFlagToaster."
     *
     * Wrapper around 'getStatus' - see that function for more details on the arguemnts.
     *
     * This wrapper checks for 'kEnabled' but raises an error for 'kNotFound' - if the flag is not
     * known. This can be useful if you want to gate an entire test on a feature flag like so:
     *   if (!FeatureFlagUtil.isEnabled(db, "myFlag")) {
     *       jsTestLog("Skipping test because my flag is not enabled");
     *       return;
     *   }
     *
     * The advantage of this throwing API is that such a test will start complaining in evergreen
     * when you delete the feature flag, rather than passing by not actually running any assertions.
     */
    function isEnabled(db, featureFlag, user, ignoreFCV) {
        let status = getStatus(db, featureFlag, user, ignoreFCV);
        assert(
            status != FlagStatus.kNotFound,
            `You asked about a feature flag ${featureFlag} which wasn't present. If this is a ` +
                "multiversion test and you want the coverage despite the flag not existing on an " +
                "older version, consider using 'isPresentAndEnabled()' instead of 'isEnabled()'");
        return status == FlagStatus.kEnabled;
    }

    /**
     *
     * Wrapper around 'getStatus' - see that function for more details on the arguemnts.
     *
     * @param 'featureFlag' - the name of the flag you want to check, but *without* the
     *     'featureFlag' prefix. For example, just "Toaster" instead of "featureFlagToaster."
     *
     * @returns true if the provided feature flag is known and also enabled. Returns false otherwise
     *     (either disabled or not known), unlike 'isEnabled()' which would raise an error if the
     *     flag is not found.
     *
     * This can be useful if you'd like to have your test conditionally add extra assertions, or
     * conditionally change the assertion being made, like so:
     *
     *   // TODO SERVER-XYZ remove 'featureFlagMyFlag'.
     *   if (FeatureFlagUtil.isPresentAndEnabled(db, "MyFlag")) {
     *       // I expect to see some new return value.
     *   } else {
     *       // I expect *not* to see some return value.
     *   }
     *
     * Note that this API should always be used with an accompanying TODO like the one in the
     * example above. This is because it is all too easy to have a test like so which will silently
     * stop testing anything if we remove the feature flag without updating the test:
     *
     *   if (FeatureFlagUtil.isPresentAndEnabled(db, "MyFlag")) {
     *       // Assert on something new.
     *   }
     *   // No else clause.
     *
     * That code is dangerous because we may forget to delete it when "featureFlagMyFlag" is
     * removed, and the test would keep passing but stop testing.
     */
    function isPresentAndEnabled(db, featureFlag, user, ignoreFCV) {
        return getStatus(db, featureFlag, user, ignoreFCV) == FlagStatus.kEnabled;
    }

    /**
     *
     * Wrapper around 'getStatus' - see that function for more details on the arguemnts.
     *
     * @param 'featureFlag' - the name of the flag you want to check, but *without* the
     *     'featureFlag' prefix. For example, just "Toaster" instead of "featureFlagToaster."
     *
     * @returns true if the provided feature flag is known and disabled. Returns false otherwise
     *     (either enabled or not known).
     *
     * This can be helpful if you want to check that a feature flag has been properly initialized
     * in your project.
     *
     *   assert(FeatureFlagUtil.isPresentAndDisabled(db, "MyFlag"))
     */
    function isPresentAndDisabled(db, featureFlag, user, ignoreFCV) {
        return getStatus(db, featureFlag, user, ignoreFCV) == FlagStatus.kDisabled;
    }

    return {
        FlagStatus: FlagStatus,
        isEnabled: isEnabled,
        isPresentAndEnabled: isPresentAndEnabled,
        isPresentAndDisabled: isPresentAndDisabled,
        getFeatureFlagDoc: getFeatureFlagDoc,
        getFeatureFlagDocStatus: getFeatureFlagDocStatus,
        getStatus: getStatus,
    };
})();
