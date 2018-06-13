/**
 * Tests parsing and validaiton of allPaths indexes.
 * @tags: [
 *  # Uses index building in background
 *  requires_background_index,
 * ]
 */
(function() {
    "use strict";
    const coll = db.getCollection("all_paths_validindex");

    const kIndexName = "all_paths_validindex";

    const createIndexAndVerifyWithDrop = (key, parameters) => {
        coll.dropIndexes();
        assert.commandWorked(coll.createIndex(key, parameters));
        assert.eq(coll.getIndexes()
                      .filter((index) => {
                          return index.name == parameters.name;
                      })
                      .length,
                  1);
    };

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
    assert.commandFailedWithCode(coll.createIndex({"$**": "hashed"}), ErrorCodes.CannotCreateIndex);

    // Cannot create a TTL all paths index.
    assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {expireAfterSeconds: 3600}),
                                 ErrorCodes.CannotCreateIndex);

    // Cannot create a geoSpatial all paths index.
    assert.commandFailedWithCode(coll.createIndex({"$**": "2dsphere"}),
                                 ErrorCodes.CannotCreateIndex);
    assert.commandFailedWithCode(coll.createIndex({"$**": "2d"}), ErrorCodes.CannotCreateIndex);

    // Cannot create a text all paths index using single sub-path syntax.
    assert.commandFailedWithCode(coll.createIndex({"a.$**": "text"}), ErrorCodes.CannotCreateIndex);

    // Cannot create a compound all paths index.
    assert.commandFailedWithCode(coll.createIndex({"$**": 1, "a": 1}),
                                 ErrorCodes.CannotCreateIndex);
    assert.commandFailedWithCode(coll.createIndex({"a": 1, "$**": 1}),
                                 ErrorCodes.CannotCreateIndex);

    // Cannot create an all paths index with an invalid spec.
    assert.commandFailedWithCode(coll.createIndex({"a.$**.$**": 1}), ErrorCodes.CannotCreateIndex);
    assert.commandFailedWithCode(coll.createIndex({"$**.$**": 1}), ErrorCodes.CannotCreateIndex);
    assert.commandFailedWithCode(coll.createIndex({"$**": "hello"}), ErrorCodes.CannotCreateIndex);
})();
