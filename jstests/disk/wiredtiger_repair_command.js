/**
 * Functional coverage for the wiredTigerRepair command. The read-only sub-commands dispatch to
 * WiredTiger's wiredtiger_repair() interface; fixDatabaseSize runs a checkpoint with a debug config.
 * This runs on a plain (non-disaggregated) standalone, so it covers what is exercisable there:
 * fetchMetadata (which reads the local metadata cursor), sub-command validation, config-injection
 * rejection, and that fixDatabaseSize is rejected off a disaggregated leader. The disaggregated
 * paths (fetchDatabaseSize, fetchMetadata local=false, a successful fixDatabaseSize) are covered
 * separately against an SLS backend.
 *
 * Lives in the disk_wiredtiger suite because the command requires the on-disk WiredTiger engine.
 *
 * @tags: [requires_wiredtiger, requires_persistence]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

let conn;
let admin;

describe("wiredTigerRepair command", function () {
    before(function () {
        conn = MongoRunner.runMongod();
        admin = conn.getDB("admin");
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("fetchMetadata returns real WiredTiger metadata", function () {
        const res = assert.commandWorked(
            admin.runCommand({wiredTigerRepair: 1, fetchMetadata: {local: true}}),
        );
        assert(res.result.includes("file:"), "expected a real metadata listing", {res});
    });

    it("fetchMetadata restricted to a URI reports that URI", function () {
        // The history store file exists on every WiredTiger instance.
        const res = assert.commandWorked(
            admin.runCommand({
                wiredTigerRepair: 1,
                fetchMetadata: {local: true, uri: "file:WiredTigerHS.wt"},
            }),
        );
        assert(res.result.includes("WiredTigerHS"), "expected the requested URI in the report", {
            res,
        });
    });

    it("requires exactly one sub-command", function () {
        assert.commandFailedWithCode(
            admin.runCommand({wiredTigerRepair: 1}),
            ErrorCodes.InvalidOptions,
            "no sub-command should be rejected",
        );
        assert.commandFailedWithCode(
            admin.runCommand({
                wiredTigerRepair: 1,
                fetchDatabaseSize: {local: true},
                fetchMetadata: {local: true},
            }),
            ErrorCodes.InvalidOptions,
            "two sub-commands should be rejected",
        );
        assert.commandFailedWithCode(
            admin.runCommand({
                wiredTigerRepair: 1,
                fetchMetadata: {local: true},
                fixDatabaseSize: true,
            }),
            ErrorCodes.InvalidOptions,
            "fetch + fix together should be rejected",
        );
    });

    it("treats fixDatabaseSize:false as not requesting the fix", function () {
        // fixDatabaseSize only counts as a sub-command when true, so this is exactly one
        // (fetchMetadata).
        assert.commandWorked(
            admin.runCommand({
                wiredTigerRepair: 1,
                fetchMetadata: {local: true},
                fixDatabaseSize: false,
            }),
        );
        // ...and false on its own is zero sub-commands.
        assert.commandFailedWithCode(
            admin.runCommand({wiredTigerRepair: 1, fixDatabaseSize: false}),
            ErrorCodes.InvalidOptions,
        );
    });

    it("rejects config-injection characters in uri/key", function () {
        assert.commandFailedWithCode(
            admin.runCommand({
                wiredTigerRepair: 1,
                fetchMetadata: {local: true, uri: 'file:foo.wt")'},
            }),
            ErrorCodes.BadValue,
        );
    });

    it("fixDatabaseSize requires a disaggregated leader", function () {
        // fixDatabaseSize runs checkpoint(debug=(database_size_fix=true)), which is only valid on a
        // disaggregated leader; WiredTiger rejects it on a plain standalone node.
        assert.commandFailed(admin.runCommand({wiredTigerRepair: 1, fixDatabaseSize: true}));
    });
});
