/**
 * Ensures {full: true} and {collHash: true} can be combined in one validate command, producing both
 * the collection hash and the work that only a full validation does.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {checkLog} from "src/mongo/shell/check_log.js";

const kValidatingCollectionLogId = 20303;

describe("validate with full and collHash", function () {
    let conn;
    let coll;

    before(function () {
        conn = MongoRunner.runMongod();
        coll = conn.getDB("test").coll;
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(
            coll.insert([
                {_id: 1, a: 1},
                {_id: 2, a: 2},
                {_id: 3, a: 3},
            ]),
        );
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("returns the collection hash alongside the full validation results", function () {
        const collHashOnly = assert.commandWorked(coll.validate({collHash: true}));
        const fullOnly = assert.commandWorked(coll.validate({full: true}));
        const fullAndCollHash = assert.commandWorked(coll.validate({full: true, collHash: true}));

        assert(collHashOnly.hasOwnProperty("all"), "collHash did not return a hash", {
            collHashOnly,
        });
        assert(!fullOnly.hasOwnProperty("all"), "full alone returned a hash", {fullOnly});

        assert(fullAndCollHash.valid, "validation failed", {fullAndCollHash});
        assert(fullAndCollHash.hasOwnProperty("all"), "full and collHash did not return a hash", {
            fullAndCollHash,
        });
        assert.eq(collHashOnly.all, fullAndCollHash.all, "hash differs when combined with full");
        assert.eq(fullOnly.nrecords, fullAndCollHash.nrecords);
        assert.eq(fullOnly.nIndexes, fullAndCollHash.nIndexes);
        assert.eq(fullOnly.keysPerIndex, fullAndCollHash.keysPerIndex);
    });

    it("computes only the hashes that collHash names", function () {
        const sha256 = assert.commandWorked(coll.validate({collHash: "sha256"}));
        assert(sha256.hasOwnProperty("all"), "sha256 did not return the SHA-256 hash", {sha256});
        assert(!sha256.hasOwnProperty("xxh3All"), "sha256 returned the XXH3 hash", {sha256});

        const xxh3 = assert.commandWorked(coll.validate({collHash: "xxh3"}));
        assert(xxh3.hasOwnProperty("xxh3All"), "xxh3 did not return the XXH3 hash", {xxh3});
        assert(!xxh3.hasOwnProperty("all"), "xxh3 returned the SHA-256 hash", {xxh3});
        assert(!xxh3.hasOwnProperty("metadata"), "xxh3 returned the metadata hash", {xxh3});

        const both = assert.commandWorked(
            coll.validate({collHash: ["sha256", "xxh3"], full: true}),
        );
        assert.eq(sha256.all, both.all);
        assert.eq(xxh3.xxh3All, both.xxh3All);

        const absent = assert.commandWorked(coll.validate({}));
        const defaulted = assert.commandWorked(coll.validate({collHash: "default"}));
        assert(!defaulted.hasOwnProperty("all"), "default returned the SHA-256 hash", {defaulted});
        assert(!absent.hasOwnProperty("xxh3All"), "absent returned the XXH3 hash", {absent});
        assert(!defaulted.hasOwnProperty("xxh3All"), "default returned the XXH3 hash", {defaulted});
        assert.eq(
            Object.keys(absent).sort(),
            Object.keys(defaulted).sort(),
            "default differs from leaving collHash out",
        );

        assert.commandFailedWithCode(
            coll.validate({collHash: ["default"]}),
            ErrorCodes.InvalidOptions,
        );
        assert.commandFailedWithCode(coll.validate({collHash: "md5"}), ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(coll.validate({collHash: []}), ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(
            coll.validate({collHash: "xxh3", hashPrefixes: []}),
            ErrorCodes.InvalidOptions,
        );
    });

    it("resolves the validate mode and collection hash settings independently", function () {
        assert.commandWorked(coll.validate({collHash: true}));
        assert.commandWorked(coll.validate({full: true}));
        assert.commandWorked(coll.validate({full: true, collHash: true}));

        checkLog.containsJson(conn, kValidatingCollectionLogId, {
            options: (o) => o.mode === "foreground" && o.collHash.sha256 === true,
        });
        checkLog.containsJson(conn, kValidatingCollectionLogId, {
            options: (o) => o.mode === "foregroundFull" && o.collHash.sha256 === false,
        });
        checkLog.containsJson(conn, kValidatingCollectionLogId, {
            options: (o) => o.mode === "foregroundFull" && o.collHash.sha256 === true,
        });
    });
});
