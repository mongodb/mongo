/**
 * Tests query settings handling for whole-cluster change streams. Such a stream runs on the
 * internal 'admin' namespace (admin.$cmd.aggregate) but is a legitimate, user-facing query shape,
 * so it is eligible for query settings and is assigned a query shape hash like any other query.
 *
 * @tags: [
 *   featureFlagAllowUserFacingQuerySettings,
 *   needs_query_settings_user_role,
 *   requires_fcv_90,
 *   requires_replication,
 *   uses_change_streams,
 *   # 'setQuerySettings' must run on mongos, but the direct-to-shard connection passthrough
 *   # redirects the shell connection to a shard, where the command is not supported.
 *   directly_against_shardsvrs_incompatible,
 * ]
 */
import {afterEach, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

describe("whole-cluster change stream query settings", () => {
    const adminDB = db.getSiblingDB("admin");
    const qsutils = new QuerySettingsUtils(db, jsTestName());

    const wholeClusterStreamCmd = {
        aggregate: 1,
        pipeline: [{$changeStream: {allChangesForCluster: true}}],
        cursor: {},
    };

    const openWholeClusterStream = () => adminDB.runCommand(wholeClusterStreamCmd);

    // Explains the running whole-cluster stream. Passthrough overrides may rewrite the change
    // stream pipeline (for example by injecting an endOfTransaction filter), and they rewrite the
    // pipeline nested in an 'explain' identically, so the explain result reflects the command that
    // the server actually executes.
    const explainWholeClusterStream = () =>
        assert.commandWorked(
            adminDB.runCommand({explain: wholeClusterStreamCmd, verbosity: "queryPlanner"}),
        );

    // Returns the query shape hash the server assigns to the running whole-cluster stream.
    const getWholeClusterStreamShapeHash = () => explainWholeClusterStream().queryShapeHash;

    // Returns the representative query of the running whole-cluster stream, i.e. the command that
    // the server actually executes, including any pipeline stages added by passthrough overrides.
    const getWholeClusterStreamRepresentativeQuery = () => ({
        ...wholeClusterStreamCmd,
        pipeline: explainWholeClusterStream().command.pipeline,
        $db: "admin",
    });

    afterEach(() => {
        qsutils.removeAllQuerySettings();
    });

    it("rejects an all-cluster stream that carries an inline reject: true", () => {
        assert.commandFailedWithCode(
            adminDB.runCommand({...wholeClusterStreamCmd, querySettings: {reject: true}}),
            ErrorCodes.QueryRejectedBySettings,
        );
    });

    it("rejects an all-cluster stream when persisted query settings reject it", () => {
        // A whole-cluster stream is assigned a query shape hash, so settings persisted for that
        // hash are applied to the running stream.
        const queryShapeHash = getWholeClusterStreamShapeHash();
        qsutils.withQuerySettings(queryShapeHash, {reject: true}, () => {
            assert.commandFailedWithCode(
                openWholeClusterStream(),
                ErrorCodes.QueryRejectedBySettings,
            );
        });
    });

    it("rejects an all-cluster stream when persisted settings for the representative query reject it", () => {
        // Persisting the settings for the representative query (rather than its query shape hash)
        // is equivalent, as the server resolves the query shape from the representative query.
        qsutils.withQuerySettings(
            getWholeClusterStreamRepresentativeQuery(),
            {reject: true},
            () => {
                assert.commandFailedWithCode(
                    openWholeClusterStream(),
                    ErrorCodes.QueryRejectedBySettings,
                );
            },
        );
    });
});
