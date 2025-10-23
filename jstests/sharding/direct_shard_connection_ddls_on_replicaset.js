/**
 * Testing direct shard connection DDLs towards a replica set with different startup options are allowed with no special permissions
 * @tags: [
 *   # The test caches authenticated connections, so we do not support stepdowns
 *   does_not_support_stepdowns,
 *   requires_fcv_83
 * ]
 */

import {afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("Check direct DDLs against replica set with different startup option are allowed without special permissions", function () {
    before(function () {
        this.createNewUserForDBAndGetDirectConn = function (user, pwd, roles, db) {
            this.rs.getPrimary().getDB(db).createUser({user: user, pwd: pwd, roles: roles});
            const userDirectConnection = new Mongo(this.rs.getPrimary().host);
            const testDBDirectConnection = userDirectConnection.getDB(db);
            assert(testDBDirectConnection.auth(user, pwd), "Authentication failed!");
            return testDBDirectConnection;
        };
    });

    beforeEach(function () {
        this.rs = new ReplSetTest({name: "rs", nodes: 3});
    });

    afterEach(function () {
        this.rs.stopSet();
    });

    it("Node started with no options", () => {
        this.rs.startSet();
        this.rs.initiate();
        const testDBDirectConnection = this.createNewUserForDBAndGetDirectConn("user", "x", ["readWrite"], "testDB");
        assert.commandWorked(testDBDirectConnection.createCollection("testColl0"));
    });

    it("Node started with --shardsvr", () => {
        this.rs.startSet({shardsvr: ""});
        this.rs.initiate();
        const testDBDirectConnection = this.createNewUserForDBAndGetDirectConn("user", "x", ["readWrite"], "testDB");
        assert.commandWorked(testDBDirectConnection.createCollection("testColl1"));
    });

    it("Node started with --configsvr and --replicaSetConfigShardMaintenanceMode", () => {
        this.rs.startSet({configsvr: "", replicaSetConfigShardMaintenanceMode: ""});
        this.rs.initiate();
        const testDBDirectConnection = this.createNewUserForDBAndGetDirectConn("user", "x", ["readWrite"], "testDB");
        assert.commandWorked(testDBDirectConnection.createCollection("testColl2"));
    });
});
