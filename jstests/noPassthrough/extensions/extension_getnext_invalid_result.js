/**
 * Ensures that an extension returning an invalid getNext result does not crash the server.
 * The host must surface this as a query-level error (ExtensionSerializationError).
 *
 * Uses the test-only failExtensionGetNextInvalidResult failpoint to corrupt the GetNextResult
 * the host sees right after the extension's get_next returns OK, so no mongot binary is needed.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   requires_fcv_90,
 * ]
 */

import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";
import {describe, it} from "jstests/libs/mochalite.js";

checkPlatformCompatibleWithExtensions();

// $readNDocuments desugars to [$produceIds, $_internalSearchIdLookup]; $produceIds is an
// extension source stage whose getNext() is driven through ExecAggStageAPI::getNext, where the
// failpoint injects the invalid Advanced byte view.
const kExtensionLib = "libread_n_documents_mongo_extension.so";
const kFailPoint = "failExtensionGetNextInvalidResult";

describe("extension getNext invalid result", function () {
    let conn;
    let admin;
    let coll;

    function withConn(testFn) {
        withExtensions(
            {[kExtensionLib]: {}},
            (c) => {
                conn = c;
                admin = conn.getDB("admin");
                const db = conn.getDB("test");
                coll = db[jsTestName()];
                coll.drop();
                for (let i = 0; i < 5; i++) {
                    assert.commandWorked(coll.insertOne({_id: i, val: i * 10}));
                }
                testFn();
            },
            ["standalone"],
        );
    }

    function setFailPoint(field, data, len, times = 1) {
        assert.commandWorked(
            admin.runCommand({
                configureFailPoint: kFailPoint,
                mode: {times},
                data: {field, data, len},
            }),
        );
    }

    function clearFailPoint() {
        assert.commandWorked(admin.runCommand({configureFailPoint: kFailPoint, mode: "off"}));
    }

    // Runs the $readNDocuments aggregation under the failpoint and asserts the outcome.
    // expectCommandFailed selects between ExtensionSerializationError and success.
    function runWithFailPoint(field, data, len, expectCommandFailed) {
        setFailPoint(field, data, len);
        const res = coll.runCommand({
            aggregate: coll.getName(),
            pipeline: [{$readNDocuments: {numDocs: 5}}],
            cursor: {},
        });
        clearFailPoint();

        // The connection must still be alive after the failpoint fires.
        assert.commandWorked(admin.runCommand({ping: 1}));

        if (expectCommandFailed) {
            assert.commandFailedWithCode(res, ErrorCodes.ExtensionSerializationError);
        } else {
            assert.commandWorked(res);
        }
    }

    it("returns ExtensionSerializationError instead of crashing when resultDocument is empty", function () {
        withConn(() => {
            // Dangling non-null pointer (0x1) with len 0 — the Rust empty-Vec fingerprint.
            runWithFailPoint("resultDocument", 1, 0, true);
        });
    });

    it("skips dangling-empty resultMetadata instead of crashing", function () {
        withConn(() => {
            runWithFailPoint("resultMetadata", 1, 0, false);
        });
    });

    it("returns ExtensionSerializationError instead of crashing when resultDocument is null with a non-zero length", function () {
        withConn(() => {
            // Null data with non-zero length exercises the `data == nullptr` branch added to
            // isEmptyByteContainer. The dangling-non-null/len==0 case above was already rejected
            // by bsonObjFromByteView()'s length check before dereferencing; this one would
            // dereference null to read the BSON header without the fix.
            runWithFailPoint("resultDocument", 0, 5, true);
        });
    });
});
