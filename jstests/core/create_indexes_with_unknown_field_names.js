/**
 * Tests that we can have unknown field names in the index spec passed to the createIndexes command
 * if 'ignoreUnknownIndexSpecFields: true' is set on the createIndexes command.
 */
(function() {
    "use strict";

    db.unknown_field_names_create_indexes.drop();
    assert.commandFailedWithCode(db.runCommand({
        createIndexes: "unknown_field_names_create_indexes",
        indexes: [{key: {x: 1}, name: "myindex", someField: "someValue"}]
    }),
                                 ErrorCodes.InvalidIndexSpecificationOption);

    assert.commandFailedWithCode(db.runCommand({
        createIndexes: "unknown_field_names_create_indexes",
        indexes: [{key: {x: 1}, name: "myindex", someField: "someValue"}],
        ignoreUnknownIndexSpecFields: false
    }),
                                 ErrorCodes.InvalidIndexSpecificationOption);

    assert.commandFailedWithCode(db.runCommand({
        createIndexes: "unknown_field_names_create_indexes",
        indexes: [{key: {x: 1}, name: "myindex", someField: "someValue"}],
        ignoreUnknownIndexSpecFields: "badValue"
    }),
                                 ErrorCodes.TypeMismatch);

    assert.commandWorked(db.runCommand({
        createIndexes: "unknown_field_names_create_indexes",
        indexes: [{key: {x: 1}, name: "myindex", someField: "someValue"}],
        ignoreUnknownIndexSpecFields: true
    }));

    // Make sure 'someField' is not in the index spec.
    let indexes = db.unknown_field_names_create_indexes.getIndexes();
    for (let index in indexes) {
        if (0 === bsonWoCompare(indexes[index].key, {x: 1})) {
            assert.eq(indexes[index].someField, undefined);
        }
    }
})();
