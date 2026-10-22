/**
 * Tests the gating of the 'showEndOfTransaction' change stream option.
 *
 * Setting the option to a value other than 'off' requires the end-of-transaction
 * feature flag to be enabled. A value of 'off' is equivalent to leaving the option unset.
 *
 * @tags: [
 *   requires_fcv_90,
 *   uses_change_streams,
 *   featureFlagChangeStreamReaderV2,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {isEndOfTransactionUsable} from "jstests/libs/query/change_stream_util.js";

describe("showEndOfTransaction gating", function () {
    const collName = jsTestName();

    const runChangeStreamCommand = (spec) => {
        const res = db.runCommand({
            aggregate: collName,
            pipeline: [{$changeStream: spec}],
            cursor: {},
        });
        if (res.ok) {
            new DBCommandCursor(db, res).close();
        }
        return res;
    };

    before(function () {
        assertDropAndRecreateCollection(db, collName);
    });

    after(function () {
        assertDropCollection(db, collName);
    });

    describe("showEndOfTransaction options", function () {
        it("rejects unknown values", function () {
            assert.commandFailedWithCode(runChangeStreamCommand({showEndOfTransaction: "bogus"}), [
                ErrorCodes.BadValue,
                ErrorCodes.IDLUnknownField,
            ]);
        });

        it("accepts 'off' and an unset option", function () {
            if (isEndOfTransactionUsable(db)) {
                assert.commandWorked(runChangeStreamCommand({showEndOfTransaction: "off"}));
            } else {
                // Mixed-version clusters without support for the new option can fail here.
                // Note that in multi-version tests hitting an already upgraded replica set binary works.
                assert.commandWorkedOrFailedWithCode(
                    runChangeStreamCommand({showEndOfTransaction: "off"}),
                    [ErrorCodes.InvalidOptions, ErrorCodes.IDLUnknownField],
                );
            }

            // No option set at all - must work everywhere.
            assert.commandWorked(runChangeStreamCommand({}));
        });

        ["whenAvailable", "required"].forEach((option) => {
            it(`accepts '${option}' value`, function () {
                if (isEndOfTransactionUsable(db)) {
                    assert.commandWorked(runChangeStreamCommand({showEndOfTransaction: option}));
                } else {
                    // v1 change stream readers in a sharded cluster fail here as well as
                    // mixed-version clusters without support for the new option.
                    assert.commandWorkedOrFailedWithCode(
                        runChangeStreamCommand({showEndOfTransaction: option}),
                        [ErrorCodes.InvalidOptions, ErrorCodes.IDLUnknownField],
                    );
                }
            });
        });
    });
});
