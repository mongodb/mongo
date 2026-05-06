// Test that setProfilingLevel on a secondary for a non-existent database does NOT create
// the database (SERVER-119744). The database should only be created via replication, and
// system.profile should be lazily created when a profiled operation runs. This also tests
// find functionality on a nonexistent secondary.
//
// @tags: [
//   queries_system_profile_collection,
//   requires_replication,
//   requires_fcv_90,
// ]

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let rst, primary, secondary, dbName, secondaryDB;

describe("setProfilingLevel on secondary with non-existent database", function () {
    before(function () {
        rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();
        rst.awaitReplication();

        primary = rst.getPrimary();
        secondary = rst.getSecondary();
        dbName = jsTestName();
        secondaryDB = secondary.getDB(dbName);
    });

    after(function () {
        rst.stopSet();
    });

    it("should not create the database when setting profiling level", function () {
        assert(
            !secondary.getDBNames().includes(dbName),
            "database should not exist on secondary before setProfilingLevel",
        );

        assert.commandWorked(secondaryDB.runCommand({profile: 2}));

        assert(
            !secondary.getDBNames().includes(dbName),
            "database should not be created by setProfilingLevel on a secondary",
        );

        const res = secondaryDB.runCommand({profile: -1});
        assert.commandWorked(res);
        assert.eq(res.was, 2, "profiling level should be stored in memory");
    });

    it("should not create the database when performing a secondary read", function () {
        // Assumed state
        const res = secondaryDB.runCommand({profile: -1});
        assert.commandWorked(res);
        assert.eq(res.was, 2, "profiling is expected to be configured to active");
        assert.eq(
            null,
            secondaryDB.system.profile.findOne(),
            "no profile events are expected to be caputred before the db exists",
        );
        assert(!secondary.getDBNames().includes(dbName), "database should not exist on secondary before read");

        // Test secondary reads
        assert.commandWorked(secondaryDB.runCommand({find: "coll", filter: {}}));
        assert(!secondary.getDBNames().includes(dbName), "database should not be created by find on a secondary");
    });

    it("should lazily create system.profile when a profiled operation runs", function () {
        assert.commandWorked(primary.getDB(dbName).createCollection("coll"));
        rst.awaitReplication();

        assert(secondary.getDBNames().includes(dbName), "database should exist on secondary after replication");

        let collNames = secondaryDB.getCollectionNames();
        assert(
            !collNames.includes("system.profile"),
            "system.profile should not exist before any profiled operation runs",
        );

        secondaryDB.coll.find({}).itcount();

        collNames = secondaryDB.getCollectionNames();
        assert(
            collNames.includes("system.profile"),
            "system.profile should be lazily created after a profiled operation",
        );

        const profileEntries = secondaryDB.system.profile.find({op: "query", ns: dbName + ".coll"}).toArray();
        assert.gt(profileEntries.length, 0, "expected at least one profile entry after reading on secondary");
    });
});
