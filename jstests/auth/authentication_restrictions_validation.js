/**
 * Verifies that the user/role management commands reject authenticationRestrictions whose
 * clientSource or serverAddress entries are not valid IP addresses or CIDR ranges.
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

// A token that is neither a valid IP address nor a CIDR range.
const kInvalidToken = "NOT_AN_IP_OR_CIDR";

describe("authenticationRestrictions address validation", function () {
    before(function () {
        this.conn = MongoRunner.runMongod();
        this.admin = this.conn.getDB("admin");
    });

    beforeEach(function () {
        this.admin.dropAllUsers();
        this.admin.dropAllRoles();
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("createUser rejects an invalid clientSource token", function () {
        assert.commandFailedWithCode(
            this.admin.runCommand({
                createUser: "repro_user",
                pwd: "pwd",
                roles: [],
                authenticationRestrictions: [{clientSource: ["10.0.0.1", kInvalidToken]}],
            }),
            ErrorCodes.BadValue,
        );
    });

    it("updateUser rejects an invalid clientSource token and does not persist it", function () {
        assert.commandWorked(
            this.admin.runCommand({createUser: "repro_user", pwd: "pwd", roles: []}),
        );

        assert.commandFailedWithCode(
            this.admin.runCommand({
                updateUser: "repro_user",
                authenticationRestrictions: [
                    {clientSource: ["10.0.0.1", kInvalidToken, "127.0.0.1"]},
                ],
            }),
            ErrorCodes.BadValue,
        );

        // The rejected restriction must not have been written to the user document.
        const doc = this.admin.system.users.findOne({user: "repro_user", db: "admin"});
        assert(
            !doc.hasOwnProperty("authenticationRestrictions"),
            "invalid authenticationRestrictions must not be persisted",
            {doc},
        );
    });

    it("updateUser rejects an invalid serverAddress token", function () {
        assert.commandWorked(
            this.admin.runCommand({createUser: "repro_user", pwd: "pwd", roles: []}),
        );

        assert.commandFailedWithCode(
            this.admin.runCommand({
                updateUser: "repro_user",
                authenticationRestrictions: [{serverAddress: [kInvalidToken]}],
            }),
            ErrorCodes.BadValue,
        );
    });

    it("createRole rejects an invalid clientSource token", function () {
        assert.commandFailedWithCode(
            this.admin.runCommand({
                createRole: "repro_user",
                roles: [],
                privileges: [],
                authenticationRestrictions: [{clientSource: [kInvalidToken]}],
            }),
            ErrorCodes.BadValue,
        );
    });

    it("updateRole rejects an invalid clientSource token and does not persist it", function () {
        assert.commandWorked(
            this.admin.runCommand({createRole: "repro_user", roles: [], privileges: []}),
        );

        assert.commandFailedWithCode(
            this.admin.runCommand({
                updateRole: "repro_user",
                authenticationRestrictions: [{clientSource: [kInvalidToken]}],
            }),
            ErrorCodes.BadValue,
        );

        const doc = this.admin.system.roles.findOne({role: "repro_user", db: "admin"});
        assert(
            !doc.hasOwnProperty("authenticationRestrictions"),
            "invalid authenticationRestrictions must not be persisted",
            {doc},
        );
    });
});
