/**
 * Tests that 'create' rejects a clusteredIndex whose 'name' is empty or contains an embedded null
 * byte.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const indexNameNull = String.fromCharCode(0).repeat(2);

let collCounter = 0;
const nextCollName = () => `clusteredColl${collCounter++}`;

describe("clustered index name containing an embedded null byte", function () {
    before(function () {
        this.conn = MongoRunner.runMongod();
        this.testDB = this.conn.getDB(jsTestName());
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    // The empty-name and null-byte rejections share an error code, so each caller states which one it expects.
    function assertNameRejected(testDB, name, expectedReason) {
        const collName = nextCollName();
        const res = testDB.createCollection(collName, {
            clusteredIndex: {key: {_id: 1}, unique: true, name},
        });
        assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);
        assert(res.errmsg.includes(expectedReason), "rejected for the wrong reason", {
            errmsg: res.errmsg,
            expectedReason,
            nameLength: name.length,
        });
        assert(
            !testDB.getCollectionNames().includes(collName),
            "bad create command left a collection behind",
            {
                collName,
            },
        );
    }

    const failReasonNullByte = "name cannot contain NUL bytes";
    const failReasonEmptyName = "name cannot be empty";

    it("is rejected when the name is a single null byte", function () {
        assertNameRejected(this.testDB, String.fromCharCode(0), failReasonNullByte);
    });

    it("is rejected when the name is two null bytes", function () {
        assert.eq(2, indexNameNull.length, "expected a two-character name");
        assertNameRejected(this.testDB, indexNameNull, failReasonNullByte);
    });

    it("is rejected when the null byte is embedded among other characters", function () {
        assertNameRejected(
            this.testDB,
            `leading${String.fromCharCode(0)}trailing`,
            failReasonNullByte,
        );
    });

    it("is rejected when the null byte is the leading character", function () {
        assertNameRejected(this.testDB, `${String.fromCharCode(0)}trailing`, failReasonNullByte);
    });

    it("is rejected when the null byte is the trailing character", function () {
        assertNameRejected(this.testDB, `leading${String.fromCharCode(0)}`, failReasonNullByte);
    });

    it("is rejected when the name is empty", function () {
        assertNameRejected(this.testDB, "", failReasonEmptyName);
    });

    it("still accepts a valid name", function () {
        const collName = nextCollName();
        assert.commandWorked(
            this.testDB.createCollection(collName, {
                clusteredIndex: {key: {_id: 1}, unique: true, name: "myClusteredIndex"},
            }),
        );
        assert.commandWorked(this.testDB.runCommand({collStats: collName}));
    });
});
