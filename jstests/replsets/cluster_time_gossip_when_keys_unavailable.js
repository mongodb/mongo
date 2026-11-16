/**
 * Tests how a node that cannot load its HMAC keys (because every key cache refresh fails)
 * treats the $clusterTime gossip it receives from clients.
 *
 * Validation of the gossip is optional metadata processing: when the node's key refresher has
 * never succeeded it must drop the gossip rather than fail the whole command, otherwise the
 * node would be unusable to any client whose driver gossips cluster times. Dropping the gossip
 * must not advance the node's cluster time, and once keys are available again gossip signed by
 * an unknown key must be rejected as usual.
 *
 * The key refresh failure is simulated with the
 * 'keyRefreshFailWithReadConcernMajorityNotAvailableYet' fail point, which makes the key
 * refresher thread fail the same way it does on a restarted node whose majority commit point
 * is unavailable (e.g. a PSA set whose primary restarted while the secondary was down). The
 * fail point only affects the key refresher used on secondaries, so the node is restarted as
 * a secondary of a two-node set.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_persistence,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Multiple users can be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

const kFailPoint = "keyRefreshFailWithReadConcernMajorityNotAvailableYet";

// This user cannot have the advanceClusterTime privilege, so its $clusterTime gossip is
// validated by the server instead of being trusted.
const kPlainUser = {
    name: "plainUser",
    pwd: "pwd",
};

describe("cluster time gossip when keys are unavailable", function () {
    let rst;
    let primary;
    let capturedClusterTime;
    // The far-future time claimed by the gossip that the node could not validate.
    let forgedTime;

    // Returns a new connection to the secondary authenticated as the given user only, so that
    // each test starts from a clean authorization state.
    function connectAs(user, pwd, dbName) {
        const db = new Mongo(rst.getSecondary().host).getDB(dbName);
        assert.eq(1, db.auth(user, pwd));
        return db;
    }

    before(function () {
        rst = new ReplSetTest({
            name: "clusterTimeGossipKeysUnavailable",
            nodes: 2,
            keyFile: "jstests/libs/key1",
        });
        rst.startSet();
        rst.initiate();

        primary = rst.getPrimary();
        const primaryAdminDB = primary.getDB("admin");
        const primaryTestDB = primary.getDB("test");

        // Create the first user via the localhost exception, then authenticate as that user.
        primaryAdminDB.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
        assert.eq(1, primaryAdminDB.auth("admin", "pwd"));
        assert.commandWorked(
            primaryTestDB.runCommand({
                createUser: kPlainUser.name,
                pwd: kPlainUser.pwd,
                roles: ["readWrite"],
            }),
        );
        primaryAdminDB.logout();
        rst.awaitReplication();

        // Capture a signed $clusterTime while the nodes have keys loaded. The plain user is the
        // only authenticated user on the connection, so the response must carry a real signature.
        assert.eq(1, primaryTestDB.auth(kPlainUser.name, kPlainUser.pwd));
        const response = assert.commandWorked(
            primaryTestDB.runCommand({insert: "coll", documents: [{_id: 0}]}),
        );
        assert(
            response.$clusterTime && response.$clusterTime.signature,
            "expected a signed $clusterTime",
            {
                response,
            },
        );
        assert(
            response.$clusterTime.signature.keyId > 0,
            "expected a real signature, not a dummy one",
            {
                response,
            },
        );
        capturedClusterTime = response.$clusterTime;
        primaryTestDB.logout();

        // Restart the secondary so its in-memory key cache is empty, with every key cache
        // refresh forced to fail. It can neither validate nor sign cluster times.
        rst.stop(1);
        rst.start(
            1,
            {setParameter: {["failpoint." + kFailPoint]: tojson({mode: "alwaysOn"})}},
            /*restart=*/ true,
        );
        rst.awaitSecondaryNodes();
        // The restarted node may still be catching up; wait until it has the users.
        rst.awaitReplication();
    });

    after(function () {
        rst.stopSet();
    });

    it("drops gossip it cannot validate instead of failing the command", function () {
        const secondaryTestDB = connectAs(kPlainUser.name, kPlainUser.pwd, "test");
        assert.commandWorked(
            secondaryTestDB.runCommand({ping: 1, $clusterTime: capturedClusterTime}),
        );
        secondaryTestDB.logout();
    });

    it("drops gossip without adopting the time it claims", function () {
        forgedTime = Timestamp(capturedClusterTime.clusterTime.t + 86400, 0);
        const forgedGossip = {
            clusterTime: forgedTime,
            signature: capturedClusterTime.signature,
        };

        const secondaryTestDB = connectAs(kPlainUser.name, kPlainUser.pwd, "test");
        assert.commandWorked(secondaryTestDB.runCommand({ping: 1, $clusterTime: forgedGossip}));
        secondaryTestDB.logout();
    });

    it("validates gossip again once the key cache can be refreshed", function () {
        const secondaryAdminDB = connectAs("admin", "pwd", "admin");
        assert.commandWorked(
            secondaryAdminDB.runCommand({configureFailPoint: kFailPoint, mode: "off"}),
        );
        secondaryAdminDB.logout();

        // The response carries the node's current clock, which proves the dropped gossip did not
        // advance the cluster time to the (far future) time it claimed.
        const secondaryTestDB = connectAs(kPlainUser.name, kPlainUser.pwd, "test");
        const response = assert.commandWorked(
            secondaryTestDB.runCommand({ping: 1, $clusterTime: capturedClusterTime}),
        );
        secondaryTestDB.logout();
        assert(
            response.$clusterTime && response.$clusterTime.signature,
            "expected a signed $clusterTime",
            {
                response,
            },
        );
        assert.lt(
            timestampCmp(response.$clusterTime.clusterTime, forgedTime),
            0,
            "the cluster time was advanced by gossip that could not be validated",
            {clusterTime: response.$clusterTime.clusterTime, forgedTime},
        );
    });

    it("rejects gossip signed by an unknown key once the key cache has been refreshed", function () {
        // Key ids are cluster timestamps, so a tiny id can never belong to a real key.
        const unknownKeyGossip = {
            clusterTime: Timestamp(capturedClusterTime.clusterTime.t + 43200, 0),
            signature: {
                hash: capturedClusterTime.signature.hash,
                keyId: NumberLong(1),
            },
        };

        const secondaryTestDB = connectAs(kPlainUser.name, kPlainUser.pwd, "test");
        assert.commandFailedWithCode(
            secondaryTestDB.runCommand({ping: 1, $clusterTime: unknownKeyGossip}),
            ErrorCodes.KeyNotFound,
        );
        secondaryTestDB.logout();
    });
});
