// Tests for the "create" command.
(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    // "create" command rejects invalid options.
    db.create_collection.drop();
    assert.commandFailedWithCode(db.createCollection("create_collection", {unknown: 1}),
                                 ErrorCodes.InvalidOptions);

    //
    // Tests for "idIndex" field.
    //

    // "idIndex" field not allowed with "viewOn".
    db.create_collection.drop();
    assert.commandWorked(db.createCollection("create_collection"));
    assert.commandFailedWithCode(db.runCommand({
        create: "create_view",
        viewOn: "create_collection",
        idIndex: {key: {_id: 1}, name: "_id_"}
    }),
                                 ErrorCodes.InvalidOptions);

    // "idIndex" field not allowed with "autoIndexId".
    db.create_collection.drop();
    assert.commandFailedWithCode(
        db.createCollection("create_collection",
                            {autoIndexId: false, idIndex: {key: {_id: 1}, name: "_id_"}}),
        ErrorCodes.InvalidOptions);

    // "idIndex" field must be an object.
    db.create_collection.drop();
    assert.commandFailedWithCode(db.createCollection("create_collection", {idIndex: 1}),
                                 ErrorCodes.TypeMismatch);

    // "idIndex" field cannot be empty.
    db.create_collection.drop();
    assert.commandFailedWithCode(db.createCollection("create_collection", {idIndex: {}}),
                                 ErrorCodes.FailedToParse);

    // "idIndex" field must be a specification for an _id index.
    db.create_collection.drop();
    assert.commandFailedWithCode(
        db.createCollection("create_collection", {idIndex: {key: {a: 1}, name: "a_1"}}),
        ErrorCodes.BadValue);

    // "idIndex" field must have "key" equal to {_id: 1}.
    db.create_collection.drop();
    assert.commandFailedWithCode(
        db.createCollection("create_collection", {idIndex: {key: {a: 1}, name: "_id_"}}),
        ErrorCodes.BadValue);

    // The name of an _id index gets corrected to "_id_".
    db.create_collection.drop();
    assert.commandWorked(
        db.createCollection("create_collection", {idIndex: {key: {_id: 1}, name: "a_1"}}));
    var indexSpec = GetIndexHelpers.findByKeyPattern(db.create_collection.getIndexes(), {_id: 1});
    assert.neq(indexSpec, null);
    assert.eq(indexSpec.name, "_id_", tojson(indexSpec));

    // "idIndex" field must only contain fields that are allowed for an _id index.
    db.create_collection.drop();
    assert.commandFailedWithCode(
        db.createCollection("create_collection",
                            {idIndex: {key: {_id: 1}, name: "_id_", sparse: true}}),
        ErrorCodes.InvalidIndexSpecificationOption);

    // "create" creates v=2 _id index when "v" is not specified in "idIndex".
    db.create_collection.drop();
    assert.commandWorked(
        db.createCollection("create_collection", {idIndex: {key: {_id: 1}, name: "_id_"}}));
    indexSpec = GetIndexHelpers.findByName(db.create_collection.getIndexes(), "_id_");
    assert.neq(indexSpec, null);
    assert.eq(indexSpec.v, 2, tojson(indexSpec));

    // "create" creates v=1 _id index when "idIndex" has "v" equal to 1.
    db.create_collection.drop();
    assert.commandWorked(
        db.createCollection("create_collection", {idIndex: {key: {_id: 1}, name: "_id_", v: 1}}));
    indexSpec = GetIndexHelpers.findByName(db.create_collection.getIndexes(), "_id_");
    assert.neq(indexSpec, null);
    assert.eq(indexSpec.v, 1, tojson(indexSpec));

    // "create" creates v=2 _id index when "idIndex" has "v" equal to 2.
    db.create_collection.drop();
    assert.commandWorked(
        db.createCollection("create_collection", {idIndex: {key: {_id: 1}, name: "_id_", v: 2}}));
    indexSpec = GetIndexHelpers.findByName(db.create_collection.getIndexes(), "_id_");
    assert.neq(indexSpec, null);
    assert.eq(indexSpec.v, 2, tojson(indexSpec));

    // "collation" field of "idIndex" must match collection default collation.
    db.create_collection.drop();
    assert.commandFailedWithCode(
        db.createCollection("create_collection",
                            {idIndex: {key: {_id: 1}, name: "_id_", collation: {locale: "en_US"}}}),
        ErrorCodes.BadValue);

    db.create_collection.drop();
    assert.commandFailedWithCode(db.createCollection("create_collection", {
        collation: {locale: "fr_CA"},
        idIndex: {key: {_id: 1}, name: "_id_", collation: {locale: "en_US"}}
    }),
                                 ErrorCodes.BadValue);

    db.create_collection.drop();
    assert.commandFailedWithCode(db.createCollection("create_collection", {
        collation: {locale: "fr_CA"},
        idIndex: {key: {_id: 1}, name: "_id_", collation: {locale: "simple"}}
    }),
                                 ErrorCodes.BadValue);

    db.create_collection.drop();
    assert.commandWorked(db.createCollection("create_collection", {
        collation: {locale: "en_US", strength: 3},
        idIndex: {key: {_id: 1}, name: "_id_", collation: {locale: "en_US"}}
    }));
    indexSpec = GetIndexHelpers.findByName(db.create_collection.getIndexes(), "_id_");
    assert.neq(indexSpec, null);
    assert.eq(indexSpec.collation.locale, "en_US", tojson(indexSpec));

    // If "collation" field is not present in "idIndex", _id index inherits collection default
    // collation.
    db.create_collection.drop();
    assert.commandWorked(db.createCollection(
        "create_collection",
        {collation: {locale: "en_US"}, idIndex: {key: {_id: 1}, name: "_id_"}}));
    indexSpec = GetIndexHelpers.findByName(db.create_collection.getIndexes(), "_id_");
    assert.neq(indexSpec, null);
    assert.eq(indexSpec.collation.locale, "en_US", tojson(indexSpec));
})();
