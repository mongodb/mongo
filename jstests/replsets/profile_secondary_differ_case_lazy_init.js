// Verify that profiling a read on a secondary for a database name that
// differs in casing from a replicated database does not trigger lazy initialization of an
// unreplicated database (which would cause a DatabaseDifferCase error).
//
// Scenario:
//   1. Secondary: setProfilingLevel(2) for "mydb" (non-existent)
//   2. Primary: create "MyDb" (different casing) — replicated to secondary
//   3. Secondary: find() on "mydb" — triggers profiling, which calls doProfile()
//
// doProfile() must not attempt to create "mydb" or "mydb.system.profile" since only "MyDb" exists.
//
// @tags: [
//   queries_system_profile_collection,
//   requires_replication,
//   requires_fcv_90,
// ]

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let rst, primary, secondary, lowerName, upperName;

describe("lazy profiling with differently-cased database", function () {
    before(function () {
        rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();
        rst.awaitReplication();

        primary = rst.getPrimary();
        secondary = rst.getSecondary();
        lowerName = jsTestName() + "_mydb";
        upperName = jsTestName() + "_MyDb";
    });

    after(function () {
        rst.stopSet();
    });

    it("should not profile find commands until profiling is set and the database is created", function () {
        const result = secondary.getDB(upperName).coll.find({}).toArray();
        assert.eq(result.length, 0, "find on non-existent collection should return empty");
        secondary.getDB(upperName).system.profile.find({}).toArray();
        assert.eq(result.length, 0, "find on non-existent profile collection should return empty");
    });

    it("should enable profiling on secondary without creating the database", function () {
        assert.commandWorked(secondary.getDB(lowerName).runCommand({profile: 2}));
        assert.commandWorked(secondary.getDB(upperName).runCommand({profile: 2}));
        assert(!secondary.getDBNames().includes(lowerName), lowerName + " should not be created by setProfilingLevel");
        assert(!secondary.getDBNames().includes(upperName), upperName + " should not be created by setProfilingLevel");
    });

    it("should not profile find commands with profiling set until the db has been created", function () {
        const result = secondary.getDB(upperName).coll.find({}).toArray();
        assert.eq(result.length, 0, "find on non-existent collection should return empty");
        secondary.getDB(upperName).system.profile.find({}).toArray();
        assert.eq(result.length, 0, "find on non-existent profile collection should return empty");
    });

    it("should replicate differently-cased database to secondary", function () {
        assert.commandWorked(primary.getDB(upperName).coll.insert({x: 1}));
        rst.awaitReplication();

        assert(secondary.getDBNames().includes(upperName), upperName + " should exist on secondary after replication");
        assert(
            !secondary.getDBNames().includes(lowerName),
            lowerName + " should not exist on secondary before the read",
        );
    });

    it("should not create unreplicated database when profiling triggers on non-existent db", function () {
        const result = secondary.getDB(lowerName).coll.find({}).toArray();
        assert.eq(result.length, 0, "find on non-existent collection should return empty");

        assert(secondary.getDBNames().includes(upperName), upperName + " should still exist on secondary");
        assert(
            !secondary.getDBNames().includes(lowerName),
            lowerName + " must not be created by the profiler's lazy initialization",
        );
    });

    it("should still replicate normally after the profiling attempt", function () {
        assert.commandWorked(primary.getDB(upperName).coll.insert({x: 2}));
        rst.awaitReplication();
        const count = secondary.getDB(upperName).coll.find({}).itcount();
        assert.eq(count, 2, "secondary should have both documents after replication");
    });

    it("should profile now that the db has been created", function () {
        const result = secondary.getDB(upperName).system.profile.find({}).toArray();
        assert.eq(result.length, 1, "find on existent, profiled collection should be nonempty");
    });
});
