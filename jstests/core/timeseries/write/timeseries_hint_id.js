/**
 * Tests running the find and delete commands with a hint on the _id index on a timeseries
 * collection. Verifies that commands specifying a hint on the _id index via "_id_" always fail.
 * @tags: [
 *   # This is a backwards-breaking change.
 *   requires_fcv_83,
 *   does_not_support_stepdowns,
 *   # Retryable deletes, for example, not supported on timeseries collections
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_commands,
 *   requires_non_retryable_writes,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
const timeFieldName = "time";
const metaFieldName = "tag";
const collName = jsTestName();
const dbName = jsTestName();

const testDB = db.getSiblingDB(dbName);
const coll = testDB.getCollection(collName);

assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
);

const objA = {
    [timeFieldName]: ISODate(),
    "measurement": {"A": "cpu"},
    [metaFieldName]: {a: "A"},
};
const objB = {
    [timeFieldName]: ISODate(),
    "measurement": {"A": "cpu"},
    [metaFieldName]: {b: "B"},
};
const objC = {
    [timeFieldName]: ISODate(),
    "measurement": {"A": "cpu"},
    [metaFieldName]: {c: "C"},
};

assert.commandWorked(coll.insert([objA, objB, objC]));

function runCommandOnTSCollection(commandWithHintObjectValue, commandWithHintStringValue) {
    // No user-facing index exists on the _id field. It only exists on the underlying buckets
    // collection.
    assert.commandFailedWithCode(testDB.runCommand(commandWithHintObjectValue), ErrorCodes.BadValue);

    assert.commandFailedWithCode(testDB.runCommand(commandWithHintStringValue), ErrorCodes.BadValue);

    // Create an index on the _id field of the user-facing view collection.
    assert.commandWorked(coll.createIndex({_id: 1}));

    // This hint will be rewritten as the key pattern (ex:
    // "hint":{"control.min._id":1,"control.max._id":1}) for the buckets collection.
    assert.commandWorked(testDB.runCommand(commandWithHintObjectValue));

    assert.commandFailedWithCode(testDB.runCommand(commandWithHintStringValue), ErrorCodes.BadValue);

    // Drop the index on the _id field of the user-facing view collection.
    assert.commandWorked(coll.dropIndex({_id: 1}));
}

(function runFindCommand() {
    runCommandOnTSCollection({find: collName, hint: {_id: 1}}, {find: collName, hint: "_id_"});
})();

(function runDeleteCommand() {
    runCommandOnTSCollection(
        {delete: collName, deletes: [{q: {metaFieldName: "b"}, limit: 0, hint: {_id: 1}}]},
        {delete: collName, deletes: [{q: {metaFieldName: "c"}, limit: 0, hint: "_id_"}]},
    );
    assert.commandWorked(coll.insert([objB, objC]));
})();

(function runAggregateCommand() {
    assert.commandWorked(coll.insert([objA]));
    const matchPipeline = [{$match: {metaFieldName: "a"}}, {$sort: {_id: 1}}, {$project: {_id: 0, time: 0}}];
    runCommandOnTSCollection(
        {
            aggregate: collName,
            pipeline: matchPipeline,
            cursor: {},
            allowDiskUse: true,
            hint: {_id: 1},
        },
        {
            aggregate: collName,
            pipeline: matchPipeline,
            cursor: {},
            allowDiskUse: true,
            hint: "_id_",
        },
    );
})();

(function runUpdateCommand() {
    runCommandOnTSCollection(
        {
            update: collName,
            updates: [
                {
                    q: {tag: "b"},
                    u: {$set: {tag: {b: "RAM"}}},
                    hint: {_id: 1},
                    multi: true,
                },
            ],
        },
        {
            update: collName,
            updates: [
                {
                    q: {tag: "c"},
                    u: {$set: {tag: {c: "L1"}}},
                    hint: "_id_",
                    multi: true,
                },
            ],
        },
    );
})();

(function runCountCommand() {
    runCommandOnTSCollection(
        {count: collName, query: {metaFieldName: "a"}, hint: {_id: 1}},
        {count: collName, query: {metaFieldName: "a"}, hint: "_id_"},
    );
})();
