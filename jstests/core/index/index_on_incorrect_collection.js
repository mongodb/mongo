/**
 * If an incompatible index exists on a collection, the server should prevent updates to that index with non-fatal errors.
 * @tags: [
 *   # Older versions are expected to have fatal failures.
 *   requires_fcv_83
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("Nonfatal error when attempting to update an improper timeseries-only index on a non-timeseries collection.", function () {
    const collName = jsTestName();
    before(function () {
        this.coll = db.getCollection(collName);
        this.coll.drop();
        assert.commandWorked(db.createCollection(collName));
    });

    it("Prevents updating 2dsphere_bucket indices for top-level measurements", function () {
        // Authorization rules will normally prevent a non-system user from creating this index.
        assert.commandWorked(this.coll.createIndex({x: "2dsphere_bucket"}));
        assert.commandFailed(this.coll.insert({control: {version: 2}, x: HexData(0, "00")}));
    });

    it("Prevents updating 2dsphere_bucket indices for nested measurements", function () {
        // Authorization rules will normally prevent a non-system user from creating this index.
        assert.commandWorked(this.coll.createIndex({"data.a.b.c": "2dsphere_bucket"}));
        assert.commandFailed(this.coll.insert({control: {version: 2}, data: {a: {b: {c: [0, 0]}}}}));
    });

    after(function () {
        this.coll.drop();
    });
});
