/**
 * Tests that an internal-only extension stage is rejected from a user pipeline but accepted from an
 * internal client, and that a user-facing stage desugaring into an internal-only stage stays usable
 * (enforcement applies to what the user wrote, not to expansion output).
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 * ]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

const kNotAllowedInUserRequestsCode = 5491300;

// Returns a fresh connection to 'host' marked as an internal client. Sending the internalClient
// field in hello is what tags the connection internal; maxWireVersion is a required subfield.
function makeInternalConn(host) {
    const conn = new Mongo(host);
    const maxWireVersion = assert.commandWorked(conn.adminCommand({hello: 1})).maxWireVersion;
    assert.commandWorked(
        conn.adminCommand({hello: 1, internalClient: {maxWireVersion: NumberInt(maxWireVersion)}}),
    );
    return conn;
}

describe("internal-only extension stage", function () {
    before(function () {
        this.coll = db.internal_only_stage_rejected;
        this.coll.drop();
        assert.commandWorked(this.coll.insert({_id: 1}));
    });

    it("is rejected from a user pipeline", function () {
        assertErrorCode(
            this.coll,
            [{$_testInternalStage: {}}],
            kNotAllowedInUserRequestsCode,
            "Specifying an internal-only extension stage in a user pipeline should be rejected",
        );
    });

    it("is accepted from an internal client", function () {
        // mongos does not honor the internalClient handshake (cluster hello ignores the field) —
        // internal traffic enters at the shard layer — so handshake the node hosting the database
        // (the shard primary on sharded fixtures, the current node otherwise).
        const host = FixtureHelpers.getPrimaryForNodeHostingDatabase(db).host;
        const conn = makeInternalConn(host);
        try {
            assert.commandWorked(
                conn.getDB(db.getName()).runCommand({
                    aggregate: this.coll.getName(),
                    pipeline: [{$_testInternalStage: {}}],
                    cursor: {},
                    // internalClient connections must send an explicit writeConcern.
                    writeConcern: {w: 1},
                }),
            );
        } finally {
            conn.close();
        }
    });

    it("does not restrict a user-facing stage that desugars into an internal-only stage", function () {
        const res = assert.commandWorked(
            db.runCommand({
                aggregate: this.coll.getName(),
                pipeline: [{$testDesugarsToInternal: {}}],
                cursor: {},
            }),
        );
        assert.eq(
            res.cursor.firstBatch.length,
            1,
            "expected the desugared pipeline to return the document",
            {res},
        );
    });
});
