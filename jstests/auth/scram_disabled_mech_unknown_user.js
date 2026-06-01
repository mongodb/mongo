/**
 * Tests that the saslSupportedMechs field returned by the hello command respects the server's
 * configured authenticationMechanisms for both known and unknown users, and that the client
 * selects only a mechanism the server will accept.
 *
 * @tags: [requires_auth]
 */
import {describe, it, before, after} from "jstests/libs/mochalite.js";

describe("saslSupportedMechs with disabled SCRAM-SHA-256", function () {
    let conn;
    let adminDB;

    before(function () {
        conn = MongoRunner.runMongod({
            auth: "",
            setParameter: "authenticationMechanisms=SCRAM-SHA-1",
        });

        // Bootstrap: use localhost bypass to create the first admin user.
        adminDB = conn.getDB("admin");
        assert.commandWorked(
            adminDB.runCommand({
                createUser: "admin",
                pwd: "password",
                roles: ["root"],
            }),
        );
        assert(adminDB.auth("admin", "password"));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("hello saslSupportedMechs for a known user excludes disabled SCRAM-SHA-256", function () {
        const res = assert.commandWorked(
            adminDB.runCommand({
                hello: 1,
                saslSupportedMechs: "admin.admin",
            }),
        );
        assert(Array.isArray(res.saslSupportedMechs), "saslSupportedMechs should be an array for a known user", {res});
        assert(
            !res.saslSupportedMechs.includes("SCRAM-SHA-256"),
            "saslSupportedMechs must not contain the disabled SCRAM-SHA-256",
            {saslSupportedMechs: res.saslSupportedMechs},
        );
        assert(
            res.saslSupportedMechs.includes("SCRAM-SHA-1"),
            "saslSupportedMechs must contain the enabled SCRAM-SHA-1",
            {saslSupportedMechs: res.saslSupportedMechs},
        );
    });

    it("hello saslSupportedMechs for an unknown user returns only enabled mechanisms", function () {
        // When the user does not exist, advertiseMechanismNamesForUser() currently returns nothing
        // (saslSupportedMechs is absent). The client then falls back to SCRAM-SHA-256 via the wire
        // version heuristic. Since SCRAM-SHA-256 is disabled, this causes an auth failure.
        //
        // The correct behavior is to return the enabled mechanisms so the client can pick one that
        // the server will actually accept.
        const res = assert.commandWorked(
            adminDB.runCommand({
                hello: 1,
                saslSupportedMechs: "admin.nonExistentUser",
            }),
        );

        // Bug: saslSupportedMechs is currently undefined for unknown users, which causes the
        // client to incorrectly select SCRAM-SHA-256 even when it is disabled.
        assert.neq(
            undefined,
            res.saslSupportedMechs,
            "saslSupportedMechs must not be absent for unknown users when mechanisms are " +
                "restricted — absence causes the client to fall back to a potentially " +
                "disabled mechanism",
        );
        assert(
            !res.saslSupportedMechs.includes("SCRAM-SHA-256"),
            "saslSupportedMechs for an unknown user must not contain the disabled SCRAM-SHA-256",
            {saslSupportedMechs: res.saslSupportedMechs},
        );
        assert(
            res.saslSupportedMechs.includes("SCRAM-SHA-1"),
            "saslSupportedMechs for an unknown user must contain the enabled SCRAM-SHA-1",
            {saslSupportedMechs: res.saslSupportedMechs},
        );
    });

    it("client mechanism selection for an unknown user must not pick a disabled mechanism", function () {
        // _getDefaultAuthenticationMechanism() falls back to SCRAM-SHA-256 (via wire version
        // heuristic) when saslSupportedMechs is absent from the hello response. This happens
        // for unknown users. With SCRAM-SHA-256 disabled on the server, any subsequent auth
        // attempt then fails with "SCRAM-SHA-256 authentication is disabled".
        //
        // Once the server-side fix is applied (returning enabled mechs for unknown users),
        // the client will receive ["SCRAM-SHA-1"] and correctly select SCRAM-SHA-1.
        const selectedMech = adminDB._getDefaultAuthenticationMechanism("nonExistentUser", "admin");
        assert.neq(
            "SCRAM-SHA-256",
            selectedMech,
            "client must not fall back to the disabled SCRAM-SHA-256 for an unknown user",
        );
        assert.eq("SCRAM-SHA-1", selectedMech, "client must select the enabled SCRAM-SHA-1 for an unknown user");
    });
});
