/**
 * A collection whose validator failed to parse retains it and rejects writes until collMod
 * replaces it. Run against standalone, replica set, and sharded fixtures.
 * @tags: [
 *   # The setup uses a mongod failpoint to retain a validator parse error.
 *   not_allowed_with_signed_security_token,
 *   does_not_support_stepdowns,
 *   # Newly added shards would not have the node-local failpoint enabled.
 *   assumes_stable_shard_list,
 *   requires_non_retryable_commands,
 *   requires_fcv_91,
 *   # Views and time series collections do not support validators.
 *   incompatible_with_views,
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */
import {
    assertCreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// Invalid because '*' is a quantifier that does not follow a repeatable item.
const malformedValidator = {email: {$regex: "^*"}};
const kValidatorParseErrorCode = 51091;
const collName = jsTestName();
let coll;
const document = {email: "x@example.com"};

describe("collection whose validator this node failed to parse", function () {
    before(function () {
        assertDropCollection(db, collName);
    });

    beforeEach(function () {
        // Mirroring sends commands to cluster roots; mongos cannot forward this mongod-only failpoint.
        this.failPoint = OverrideHelpers.withPreOverrideRunCommand(() => {
            const clusterDbs = TestData.fixtureClusterConnectionStrings
                ? TestData.fixtureClusterConnectionStrings.map((uri) => connect(uri))
                : [db];
            const primaries = clusterDbs.flatMap((clusterDb) =>
                FixtureHelpers.mapOnEachShardNode({
                    db: clusterDb,
                    primaryNodeOnly: true,
                    func: (primaryDb) => primaryDb.getMongo(),
                }),
            );
            // Retain the parse error so writes fail; the default bypasses validator parsing.
            return configureFailPointForRS(primaries, "allowSettingMalformedCollectionValidators", {
                parseValidator: true,
            });
        });

        coll = assertCreateCollection(db, collName, {validator: malformedValidator});
    });

    afterEach(function () {
        try {
            // Migrations may recreate the malformed validator until the collection is dropped.
            assertDropCollection(db, collName);
        } finally {
            OverrideHelpers.withPreOverrideRunCommand(() => this.failPoint?.off());
        }
    });

    it("keeps the broken validator and fails writes to that collection", function () {
        assert.docEq(coll.getMetadata().options.validator, malformedValidator);
        assert.commandFailedWithCode(coll.insert(document), kValidatorParseErrorCode);
    });

    it("still accepts writes to other collections", function () {
        const unrelated = db[jsTestName() + "_unrelated"];

        try {
            assert.commandWorked(unrelated.insert(document));
        } finally {
            assertDropCollection(db, unrelated.getName());
        }
    });

    describe("after replacing the malformed validator with collMod", function () {
        beforeEach(function () {
            assert.docEq(coll.getMetadata().options.validator, malformedValidator);
            assert.commandFailedWithCode(coll.insert(document), kValidatorParseErrorCode);

            const validValidator = {email: {$regex: "^[^@]+@[^@]+$"}};
            assert.commandWorked(
                db.runCommand({collMod: coll.getName(), validator: validValidator}),
            );
            assert.docEq(coll.getMetadata().options.validator, validValidator);
        });

        it("accepts valid writes", function () {
            assert.commandWorked(coll.insert(document));
            assert.eq(coll.countDocuments({}), 1);
        });

        it("rejects invalid writes", function () {
            assert.commandFailedWithCode(
                coll.insert({email: "x"}),
                ErrorCodes.DocumentValidationFailure,
            );
            assert.eq(coll.countDocuments({}), 0);
        });
    });
});
