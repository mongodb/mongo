/**
 * Regression test for SERVER-137527.
 *
 * A bulkWrite insert into a non-existing system.buckets namespace implicitly creates that collection
 * with no timeseries options. bulkWrite's command namespace is the database, so the check that
 * rejects direct system.buckets access for commands such as insert does not see the target
 * namespace. userCreateNS must reject that malformed create with IllegalOperation. When viewless
 * timeseries are enabled, that check used to run after the legacy-format tripwire, so this insert
 * logged tripwire 12392800.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: bulkWrite.
 *   not_allowed_with_signed_security_token,
 *   command_not_supported_in_serverless,
 *   requires_fcv_90,
 *   featureFlagCreateViewlessTimeseriesCollections,
 *   # The expected failure is a write error. Transaction passthroughs abort and retry on that.
 *   does_not_support_transactions,
 *   # Asserts the mongod bulkWrite write-error shape. The tripwire this guards lives in mongod.
 *   assumes_against_mongod_not_mongos,
 *   # A timeseries CRUD passthrough would rewrite this write onto the main timeseries namespace.
 *   exclude_from_timeseries_crud_passthrough,
 *   # Multiversion suites run a pre-change binary. It still runs the legacy-format tripwire
 *   # before the missing-options check, so this insert returns Location12392800 instead of
 *   # IllegalOperation.
 *   multiversion_incompatible,
 * ]
 */

import {cursorSizeValidator, summaryFieldsValidator} from "jstests/libs/bulk_write_utils.js";
import {describe, it} from "jstests/libs/mochalite.js";

const testDB = db.getSiblingDB(jsTestName());
const bucketsCollName = "system.buckets.foo";
const bucketsNs = `${testDB.getName()}.${bucketsCollName}`;

describe("bulkWrite implicit create of a system.buckets namespace", function () {
    it("rejects the insert with IllegalOperation and does not create the collection", function () {
        const res = assert.commandWorked(
            db.adminCommand({
                bulkWrite: 1,
                ops: [{insert: 0, document: {x: 1}}],
                nsInfo: [{ns: bucketsNs}],
            }),
        );

        summaryFieldsValidator(res, {
            nErrors: 1,
            nInserted: 0,
            nDeleted: 0,
            nMatched: 0,
            nModified: 0,
            nUpserted: 0,
        });
        cursorSizeValidator(res, 1);

        // Insert write errors from this path carry ok/idx/n/code/errmsg and omit nModified.
        const entry = res.cursor.firstBatch[0];
        assert.eq(entry.ok, 0, "write should fail", {entry});
        assert.eq(entry.idx, 0, "write error index", {entry});
        assert.eq(entry.n, 0, "no documents inserted", {entry});
        assert.eq(
            entry.code,
            ErrorCodes.IllegalOperation,
            "missing timeseries options must be rejected before the legacy-format tripwire",
            {entry},
        );
        assert(
            entry.errmsg.includes(
                "Creation of a timeseries bucket collection without timeseries options is not allowed",
            ),
            "bulkWrite write error did not report the missing timeseries options rejection",
            {entry},
        );
        assert.eq(
            0,
            testDB.getCollectionInfos({name: bucketsCollName}).length,
            "implicit create of a system.buckets namespace must not succeed",
        );
    });
});
