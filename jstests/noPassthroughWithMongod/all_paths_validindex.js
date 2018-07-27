/**
 * Tests parsing and validation of allPaths indexes.
 * @tags: [
 *  # Uses index building in background
 *  requires_background_index,
 * ]
 * TODO: SERVER-36198: Move this test back to jstests/core/
 */
(function() {
    "use strict";

    const kCollectionName = "all_paths_validindex";
    const coll = db.getCollection(kCollectionName);

    const kIndexName = "all_paths_validindex";

    const createIndexHelper = function(key, parameters) {
        return db.runCommand(
            {createIndexes: kCollectionName, indexes: [Object.assign({key: key}, parameters)]});
    };

    const createIndexAndVerifyWithDrop = function(key, parameters) {
        coll.dropIndexes();
        createIndexHelper(key, parameters);
        assert.eq(coll.getIndexes()
                      .filter((index) => {
                          return index.name == parameters.name;
                      })
                      .length,
                  1);
    };

    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: true}));
    try {
        // Can create a valid allPaths index.
        createIndexAndVerifyWithDrop({"$**": 1}, {name: kIndexName});

        // Can create a valid allPaths index with subpaths.
        createIndexAndVerifyWithDrop({"a.$**": 1}, {name: kIndexName});

        // Can create an allPaths index with partialFilterExpression.
        createIndexAndVerifyWithDrop({"$**": 1},
                                     {name: kIndexName, partialFilterExpression: {a: {"$gt": 0}}});

        // Can create an allPaths index with foreground & background construction.
        createIndexAndVerifyWithDrop({"$**": 1}, {background: false, name: kIndexName});
        createIndexAndVerifyWithDrop({"$**": 1}, {background: true, name: kIndexName});

        // Can create an allPaths index with index level collation.
        createIndexAndVerifyWithDrop({"$**": 1}, {collation: {locale: "fr"}, name: kIndexName});

        // Can create an allPaths index with an inclusion projection.
        createIndexAndVerifyWithDrop({"$**": 1},
                                     {starPathsTempName: {a: 1, b: 1, c: 1}, name: kIndexName});
        // Can create an allPaths index with an exclusion projection.
        createIndexAndVerifyWithDrop({"$**": 1},
                                     {starPathsTempName: {a: 0, b: 0, c: 0}, name: kIndexName});
        // Can include _id in an exclusion.
        createIndexAndVerifyWithDrop(
            {"$**": 1}, {starPathsTempName: {_id: 1, a: 0, b: 0, c: 0}, name: kIndexName});
        // Can exclude _id in an exclusion.
        createIndexAndVerifyWithDrop(
            {"$**": 1}, {starPathsTempName: {_id: 0, a: 1, b: 1, c: 1}, name: kIndexName});

        // Cannot create an allPaths index with sparse option.
        coll.dropIndexes();
        assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {sparse: true}),
                                     ErrorCodes.CannotCreateIndex);

        // Cannot create an allPaths index with a v0 or v1 index.
        assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {v: 0}),
                                     ErrorCodes.CannotCreateIndex);
        assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {v: 1}),
                                     ErrorCodes.CannotCreateIndex);

        // Cannot create a unique index.
        assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {unique: true}),
                                     ErrorCodes.CannotCreateIndex);

        // Cannot create a hashed all paths index.
        assert.commandFailedWithCode(coll.createIndex({"$**": "hashed"}),
                                     ErrorCodes.CannotCreateIndex);

        // Cannot create a TTL all paths index.
        assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {expireAfterSeconds: 3600}),
                                     ErrorCodes.CannotCreateIndex);

        // Cannot create a geoSpatial all paths index.
        assert.commandFailedWithCode(coll.createIndex({"$**": "2dsphere"}),
                                     ErrorCodes.CannotCreateIndex);
        assert.commandFailedWithCode(coll.createIndex({"$**": "2d"}), ErrorCodes.CannotCreateIndex);

        // Cannot create a text all paths index using single sub-path syntax.
        assert.commandFailedWithCode(coll.createIndex({"a.$**": "text"}),
                                     ErrorCodes.CannotCreateIndex);

        // Cannot specify plugin by string.
        assert.commandFailedWithCode(coll.createIndex({"a": "allPaths"}),
                                     ErrorCodes.CannotCreateIndex);
        assert.commandFailedWithCode(coll.createIndex({"$**": "allPaths"}),
                                     ErrorCodes.CannotCreateIndex);

        // Cannot create a compound all paths index.
        assert.commandFailedWithCode(coll.createIndex({"$**": 1, "a": 1}),
                                     ErrorCodes.CannotCreateIndex);
        assert.commandFailedWithCode(coll.createIndex({"a": 1, "$**": 1}),
                                     ErrorCodes.CannotCreateIndex);

        // Cannot create an all paths index with an invalid spec.
        assert.commandFailedWithCode(coll.createIndex({"a.$**.$**": 1}),
                                     ErrorCodes.CannotCreateIndex);
        assert.commandFailedWithCode(coll.createIndex({"$**.$**": 1}),
                                     ErrorCodes.CannotCreateIndex);
        assert.commandFailedWithCode(coll.createIndex({"$**": "hello"}),
                                     ErrorCodes.CannotCreateIndex);

        // Cannot create an all paths index with mixed inclusion exclusion.
        assert.commandFailedWithCode(
            createIndexHelper({"$**": 1}, {name: kIndexName, starPathsTempName: {a: 1, b: 0}}),
            40178);
        // Cannot create an all paths index with computed fields.
        assert.commandFailedWithCode(
            createIndexHelper({"$**": 1},
                              {name: kIndexName, starPathsTempName: {a: 1, b: "string"}}),
            ErrorCodes.FailedToParse);
        // Cannot create an all paths index with an empty projection.
        assert.commandFailedWithCode(
            createIndexHelper({"$**": 1}, {name: kIndexName, starPathsTempName: {}}),
            ErrorCodes.FailedToParse);
        // Cannot create another index type with "starPathsTempName" projection.
        assert.commandFailedWithCode(
            createIndexHelper({"a": 1}, {name: kIndexName, starPathsTempName: {a: 1, b: 1}}),
            ErrorCodes.BadValue);
        // Cannot create a text index with a "starPathsTempName" projection.
        assert.commandFailedWithCode(
            createIndexHelper({"$**": "text"}, {name: kIndexName, starPathsTempName: {a: 1, b: 1}}),
            ErrorCodes.BadValue);
        // Cannot create an all paths index with a non-object "starPathsTempName" projection.
        assert.commandFailedWithCode(
            createIndexHelper({"a.$**": 1}, {name: kIndexName, starPathsTempName: "string"}),
            ErrorCodes.TypeMismatch);
        // Cannot exclude an subfield of _id in an inclusion.
        assert.commandFailedWithCode(createIndexHelper({"_id.id": 0, a: 1, b: 1, c: 1}),
                                     ErrorCodes.CannotCreateIndex);
        // Cannot include an subfield of _id in an exclusion.
        assert.commandFailedWithCode(createIndexHelper({"_id.id": 1, a: 0, b: 0, c: 0}),
                                     ErrorCodes.CannotCreateIndex);

        // Cannot specify both a subpath and a projection.
        assert.commandFailedWithCode(
            createIndexHelper({"a.$**": 1}, {name: kIndexName, starPathsTempName: {a: 1}}),
            ErrorCodes.FailedToParse);
        assert.commandFailedWithCode(
            createIndexHelper({"a.$**": 1}, {name: kIndexName, starPathsTempName: {b: 0}}),
            ErrorCodes.FailedToParse);
    } finally {
        assert.commandWorked(
            db.adminCommand({"setParameter": 1, "internalQueryAllowAllPathsIndexes": false}));
    }
})();
