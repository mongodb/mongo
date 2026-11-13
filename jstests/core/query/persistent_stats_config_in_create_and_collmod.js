/**
 * Tests the 'persistentStatsConfig' collection option on both 'create' and 'collMod'. The option
 * configures the persistent statistics collected for a collection, which are used by the cost-based
 * ranker and the join optimizer.
 *
 * @tags: [
 *   # TODO(SERVER-138605): Remove once persistentStatsConfig is stripped from collection options on
 *   # FCV downgrade.
 *   cannot_run_during_upgrade_downgrade,
 *   featureFlagPersistentStatsCatalog,
 * ]
 */
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {readPersistentStatsConfig} from "jstests/libs/query/persistent_samples_utils.js";

const collName = jsTestName();

const createWithConfig = (config) => db.createCollection(collName, {persistentStatsConfig: config});
const collModWithConfig = (config) =>
    db.runCommand({collMod: collName, persistentStatsConfig: config});

describe("persistentStatsConfig on create", function () {
    beforeEach(function () {
        assertDropCollection(db, collName);
    });

    afterEach(function () {
        assertDropCollection(db, collName);
    });

    it("persists the configuration", function () {
        const config = {samples: {sampleSize: 1000, joinSampleSize: 2000}};
        assert.commandWorked(createWithConfig(config));
        assert.docEq(readPersistentStatsConfig(db, collName), config);
    });

    it("persists a single sample size", function () {
        const config = {samples: {joinSampleSize: 500}};
        assert.commandWorked(createWithConfig(config));
        assert.docEq(readPersistentStatsConfig(db, collName), config);
    });

    it("rejects an empty configuration", function () {
        assert.commandFailedWithCode(createWithConfig({}), ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(createWithConfig({samples: {}}), ErrorCodes.InvalidOptions);
    });

    it("rejects non-positive sample sizes", function () {
        assert.commandFailedWithCode(
            createWithConfig({samples: {sampleSize: 0}}),
            ErrorCodes.BadValue,
        );
        assert.commandFailedWithCode(
            createWithConfig({samples: {joinSampleSize: -1}}),
            ErrorCodes.BadValue,
        );
    });

    it("rejects unknown fields", function () {
        assert.commandFailedWithCode(
            createWithConfig({samples: {sampleSize: 100, unknownField: 1}}),
            ErrorCodes.IDLUnknownField,
        );
    });
});

describe("persistentStatsConfig on collMod", function () {
    beforeEach(function () {
        assertDropCollection(db, collName);
        assert.commandWorked(db.createCollection(collName));
    });

    afterEach(function () {
        assertDropCollection(db, collName);
    });

    it("persists the configuration", function () {
        const config = {samples: {sampleSize: 1000, joinSampleSize: 2000}};
        assert.commandWorked(collModWithConfig(config));
        assert.docEq(readPersistentStatsConfig(db, collName), config);
    });

    it("updates a sample size without changing the other", function () {
        assert.commandWorked(
            collModWithConfig({samples: {sampleSize: 1000, joinSampleSize: 2000}}),
        );
        assert.commandWorked(collModWithConfig({samples: {sampleSize: 384}}));
        assert.docEq(readPersistentStatsConfig(db, collName), {
            samples: {sampleSize: 384, joinSampleSize: 2000},
        });
    });

    it("removes a sample size without changing the other", function () {
        assert.commandWorked(
            collModWithConfig({samples: {sampleSize: 1000, joinSampleSize: 2000}}),
        );
        assert.commandWorked(collModWithConfig({samples: {sampleSize: null}}));
        assert.docEq(readPersistentStatsConfig(db, collName), {samples: {joinSampleSize: 2000}});
    });

    it("removes a sample size that is not set", function () {
        assert.commandWorked(collModWithConfig({samples: {joinSampleSize: 2000}}));
        assert.commandWorked(collModWithConfig({samples: {sampleSize: null}}));
        assert.docEq(readPersistentStatsConfig(db, collName), {samples: {joinSampleSize: 2000}});
    });

    it("unsets the configuration when the last sample size is removed", function () {
        assert.commandWorked(
            collModWithConfig({samples: {sampleSize: 1000, joinSampleSize: 2000}}),
        );
        assert.commandWorked(
            collModWithConfig({samples: {sampleSize: null, joinSampleSize: null}}),
        );
        assert.eq(readPersistentStatsConfig(db, collName), undefined);
    });

    it("rejects a delta that specifies no sample size", function () {
        assert.commandFailedWithCode(collModWithConfig({}), ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(collModWithConfig({samples: {}}), ErrorCodes.InvalidOptions);
    });

    it("rejects non-positive sample sizes", function () {
        assert.commandFailedWithCode(
            collModWithConfig({samples: {sampleSize: 0}}),
            ErrorCodes.BadValue,
        );
        assert.commandFailedWithCode(
            collModWithConfig({samples: {joinSampleSize: -1}}),
            ErrorCodes.BadValue,
        );
    });

    it("rejects unknown fields", function () {
        assert.commandFailedWithCode(
            collModWithConfig({samples: {sampleSize: 100, unknownField: 1}}),
            ErrorCodes.IDLUnknownField,
        );
    });
});

describe("persistentStatsConfig on views", function () {
    const backingName = collName + "_backing";
    const viewName = collName + "_view";

    beforeEach(function () {
        assertDropCollection(db, viewName);
        assertDropCollection(db, backingName);
    });

    afterEach(function () {
        assertDropCollection(db, viewName);
        assertDropCollection(db, backingName);
    });

    it("is not supported on create", function () {
        assert.commandFailedWithCode(
            db.createCollection(viewName, {
                viewOn: backingName,
                pipeline: [],
                persistentStatsConfig: {samples: {sampleSize: 100}},
            }),
            ErrorCodes.InvalidOptions,
        );
    });

    it("is not supported on collMod", function () {
        assert.commandWorked(db.createCollection(backingName));
        assert.commandWorked(db.createView(viewName, backingName, []));
        assert.commandFailedWithCode(
            db.runCommand({collMod: viewName, persistentStatsConfig: {samples: {sampleSize: 100}}}),
            ErrorCodes.InvalidOptions,
        );
    });
});
