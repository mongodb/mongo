/**
 * Verifies that the `options` field returned by listCollections can be fed back into the create
 * command to recreate a collection with the same options, and that doing so is idempotent.
 *
 * For each case in TEST_CASES the test:
 *   1. Creates the collection with the user-supplied options.
 *   2. Snapshots the listCollections entry.
 *   3. Calls create again using the options reported by listCollections (should succeed, because
 *      create is idempotent when options match exactly).
 *   4. Verifies listCollections still reports the same options/type.
 *   5. Calls create once more with the same reported options (second idempotent recreation).
 *
 * Add new cases by appending to TEST_CASES. Use `skipOnImplicitSharding: true` for options that are
 * incompatible with sharded collections (e.g. capped).
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {beforeEach, describe, it} from "jstests/libs/mochalite.js";

const testDb = db.getSiblingDB(jsTestName());

const isImplicitlyShardedCollection = typeof globalThis.ImplicitlyShardAccessCollSettings !== "undefined";

const TEST_CASES = [
    {
        name: "plain",
        options: {},
    },
    {
        name: "capped",
        options: {capped: true, size: 1024 * 1024, max: 100},
        skipOnImplicitSharding: true,
    },
    {
        name: "validator",
        options: {
            validator: {$jsonSchema: {required: ["a"], properties: {a: {bsonType: "int"}}}},
            validationLevel: "moderate",
            validationAction: "warn",
        },
    },
    {
        name: "collation",
        options: {collation: {locale: "en_US", strength: 2}},
    },
    {
        name: "clustered",
        options: {clusteredIndex: {key: {_id: 1}, unique: true}},
    },
    {
        name: "change_stream_pre_and_post_images",
        options: {changeStreamPreAndPostImages: {enabled: true}},
    },
    {
        name: "timeseries_basic",
        options: {
            timeseries: {
                timeField: "ts",
                metaField: "meta",
                granularity: "seconds",
                bucketMaxSpanSeconds: 3600,
            },
        },
    },
    {
        name: "timeseries_custom_bucketing",
        options: {
            timeseries: {
                timeField: "ts",
                bucketMaxSpanSeconds: 100,
                bucketRoundingSeconds: 100,
            },
        },
    },
    {
        name: "timeseries_expire_after_seconds",
        options: {
            timeseries: {timeField: "ts"},
            expireAfterSeconds: 60,
        },
    },
];

function listCollectionEntry(db, name) {
    const entries = new DBCommandCursor(db, db.runCommand({listCollections: 1, filter: {name}})).toArray();
    assert.eq(1, entries.length, `Expected exactly one listCollections entry for '${name}', got: ${tojson(entries)}`);
    return entries[0];
}

function recreateFromOptions(db, name, reportedOptions, label) {
    assert.commandWorked(
        db.runCommand({create: name, ...reportedOptions}),
        `${label}: re-create from listCollections options failed for '${name}' with options ${tojson(reportedOptions)}`,
    );
}

describe("ListCollectionsCreateRoundtrip", function () {
    beforeEach(() => {
        testDb.dropDatabase();
    });

    for (const testCase of TEST_CASES) {
        it(`roundtrips '${testCase.name}'`, () => {
            if (testCase.skipOnImplicitSharding && isImplicitlyShardedCollection) {
                jsTest.log.info(`Skipping '${testCase.name}' under implicit sharding`);
                return;
            }

            const collName = testCase.name;

            // 1. Initial create with the user-supplied options.
            assert.commandWorked(
                testDb.runCommand({create: collName, ...testCase.options}),
                `Initial create failed for '${collName}'`,
            );

            // 2. Snapshot listCollections output.
            const initialEntry = listCollectionEntry(testDb, collName);
            const reportedOptions = initialEntry.options;
            jsTest.log.info(`listCollections snapshot for '${collName}'`, {entry: initialEntry});

            // 3. Re-create using the options returned by listCollections. Must succeed — create is
            //    idempotent only when every option matches exactly, so this doubles as a check that
            //    listCollections emits options that create accepts and interprets identically.
            recreateFromOptions(testDb, collName, reportedOptions, "first recreate");

            const secondEntry = listCollectionEntry(testDb, collName);
            assert.eq(initialEntry.type, secondEntry.type, `Type diverged after first recreate for '${collName}'`);
            assert.docEq(
                initialEntry.options,
                secondEntry.options,
                `Options diverged after first recreate for '${collName}'`,
            );

            // 4. Run the re-create once more to verify idempotency across repeated calls.
            recreateFromOptions(testDb, collName, reportedOptions, "second recreate");

            const thirdEntry = listCollectionEntry(testDb, collName);
            assert.eq(initialEntry.type, thirdEntry.type, `Type diverged after second recreate for '${collName}'`);
            assert.docEq(
                initialEntry.options,
                thirdEntry.options,
                `Options diverged after second recreate for '${collName}'`,
            );
        });
    }
});
