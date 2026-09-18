/**
 * Test the handling of the _id field in various contexts in various DDL operations
 *
 * @tags: [
 *   # timeseries collections do not have an index on _id
 *   exclude_from_timeseries_crud_passthrough,
 *   # Collation is lost when a view is created over a base collection
 *   incompatible_with_views,
 *   # Test uses commands that cannot be blindly retried.
 *   requires_non_retryable_commands,
 *   requires_non_retryable_writes,
 *   does_not_support_retryable_writes,
 *   # Test does not survive cluster disruptions
 *   does_not_support_stepdowns,
 *   # Fails in change_streams_pre_images_replica_sets_kill_secondary_jscore_passthrough
 *   incompatible_with_preimages_by_default
 * ]
 */

import {ClusteredCollectionUtil} from "jstests/libs/clustered_collections/clustered_collection_util.js";

db.dropDatabase();

/*
 * ==============================
 * collMod()
 * ==============================
 */

db.coll_mod.insert({a: 1});

// MongoServerError: can't hide _id     index
assert.commandFailedWithCode(
    db.runCommand({collMod: "coll_mod", index: {keyPattern: {_id: 1}, hidden: true}}),
    [ErrorCodes.BadValue, 6011800], // The 'hidden' option is not supported for a clusteredIndex
);

// MongoServerError: the _id field does not support TTL indexes
assert.commandFailedWithCode(
    db.runCommand({collMod: "coll_mod", index: {keyPattern: {_id: 1}, expireAfterSeconds: 1}}),
    [ErrorCodes.InvalidOptions, ErrorCodes.IndexNotFound],
);

// prepareUnique - we can freely set prepareUnique to true or false with no ill effects

if (!ClusteredCollectionUtil.isCollectionClustered(db.coll_mod)) {
    assert.commandWorked(
        db.runCommand({collMod: "coll_mod", index: {keyPattern: {_id: 1}, prepareUnique: true}}),
    );

    db.coll_mod.insert({a: 2});
    assert.commandWorked(
        db.runCommand({collMod: "coll_mod", index: {keyPattern: {_id: 1}, prepareUnique: false}}),
    );
    db.coll_mod.insert({a: 3});

    // unique
    assert.commandFailedWithCode(
        db.runCommand({collMod: "coll_mod", index: {keyPattern: {_id: 1}, unique: false}}),
        ErrorCodes.BadValue,
    );
    assert.commandWorked(
        db.runCommand({collMod: "coll_mod", index: {keyPattern: {_id: 1}, unique: true}}),
    );
}

// validationAction directly on _id
assert.commandWorked(
    db.runCommand({
        collMod: "coll_mod",
        validator: {_id: {$type: "string"}},
        validationAction: "error",
    }),
);
assert.commandFailedWithCode(db.coll_mod.insert({_id: 123}), ErrorCodes.DocumentValidationFailure);
assert.commandFailedWithCode(db.coll_mod.insert({a: 1}), ErrorCodes.DocumentValidationFailure);

// validationAction that references _id
assert.commandWorked(db.runCommand({collMod: "coll_mod", validator: {}}));
assert.commandWorked(
    db.runCommand({collMod: "coll_mod", validator: {a: {$eq: "$_id"}}, validationAction: "error"}),
);
assert.commandFailedWithCode(db.coll_mod.insert({a: 123}), ErrorCodes.DocumentValidationFailure);

assert.commandWorked(db.runCommand({collMod: "coll_mod", validator: {}}));
assert.commandWorked(
    db.runCommand({
        collMod: "coll_mod",
        validator: {$expr: {$eq: ["$a", "$_id"]}},
        validationAction: "error",
    }),
);
assert.commandFailedWithCode(db.coll_mod.insert({a: 123}), ErrorCodes.DocumentValidationFailure);

/*
 * ==============================
 * createCollection()
 * ==============================
 */

// createCollection() + clusteredIndex - we can't create a clustered index on _id

assert.commandFailedWithCode(
    db.createCollection("clustered_index", {clusteredIndex: {key: {a: 1}, unique: true}}),
    ErrorCodes.InvalidIndexSpecificationOption,
);
assert.commandFailedWithCode(
    db.createCollection("clustered_index", {clusteredIndex: {key: {_id: -1}, unique: true}}),
    ErrorCodes.InvalidIndexSpecificationOption,
);
// The clusteredIndex option requires unique: true to be specified
assert.commandFailedWithCode(
    db.createCollection("clustered_index", {clusteredIndex: {key: {_id: 1}, unique: false}}),
    5979700,
);
assert.commandFailedWithCode(
    db.createCollection("clustered_index", {clusteredIndex: {key: {_id: 1, b: 1}, unique: true}}),
    ErrorCodes.InvalidIndexSpecificationOption,
);

// createCollection() + encryptedFields - we can't encrypt _id or its subfields

const keyId = UUID("11111111-2222-3333-4444-555555555555"); // your data key id from the key vault
const buildInfo = assert.commandWorked(db.runCommand({buildInfo: 1}));
const isEnterprise = buildInfo.modules.includes("enterprise");

let res = db.createCollection("encrypted_fields", {
    encryptedFields: {
        "fields": [
            {
                "keyId": keyId,
                "path": "_id",
                "bsonType": "string",
            },
        ],
    },
});
if (isEnterprise) {
    assert.commandFailedWithCode(res, 6316403); // Cannot encrypt _id or its subfields
} else {
    assert.commandFailed(res);
}

res = db.createCollection("encrypted_fields", {
    encryptedFields: {
        "fields": [
            {
                "keyId": keyId,
                "path": "_id.a",
                "bsonType": "string",
            },
        ],
    },
});
if (isEnterprise) {
    assert.commandFailedWithCode(res, 6316403); // Cannot encrypt _id or its subfields
} else {
    assert.commandFailed(res);
}

// createCollection() + locale: the _id index remains case_insensitive

assert.commandWorked(
    db.createCollection("create_case_insensitive", {
        collation: {locale: "en", strength: 2},
    }),
);

assert.commandWorked(db.create_case_insensitive.insertOne({id: "a"}));
assert.commandWorked(db.create_case_insensitive.insertOne({id: "A"}));

// Assert that both rows are returned on {id: "a"}
assert.eq(db.create_case_insensitive.find({id: "a"}).toArray().length, 2);

/*
 * ==============================
 * createIndex()
 * ==============================
 */

db.create_index.insert({});

assert.commandFailedWithCode(db.create_index.createIndex({_id: -1}), ErrorCodes.BadValue);

// Creating a hashed index on _id is allowed, but it provides no benefit and will not be chosen
assert.commandWorked(db.create_index.createIndex({_id: "hashed"}));
db.create_index.insert({_id: 1});

assert.commandFailedWithCode(
    db.create_index.createIndex({_id: 1}, {unique: false}),
    ErrorCodes.InvalidIndexSpecificationOption,
);
assert.commandFailedWithCode(
    db.create_index.createIndex({a: 1}, {name: "_id_"}),
    ErrorCodes.BadValue,
);
assert.commandFailedWithCode(
    db.create_index.createIndex({_id: 1}, {sparse: true}),
    ErrorCodes.InvalidIndexSpecificationOption,
);
assert.commandFailedWithCode(
    db.create_index.createIndex({_id: 1}, {expireAfterSeconds: 3600}),
    ErrorCodes.InvalidIndexSpecificationOption,
);
assert.commandFailedWithCode(
    db.create_index.createIndex({_id: 1}, {hidden: true}),
    ErrorCodes.InvalidIndexSpecificationOption,
);

// createIndex() + partialFilterExpression

assert.commandWorked(
    db.partial_filter_expression.createIndex({a: 1}, {partialFilterExpression: {_id: 1}}),
);

assert.commandWorked(db.partial_filter_expression.insertOne({_id: 1, a: 1}));
assert.commandWorked(db.partial_filter_expression.insertOne({_id: 2, a: 1}));

assert.eq(db.partial_filter_expression.find({_id: 1, a: 1}).toArray().length, 1);
assert.eq(db.partial_filter_expression.find({_id: 1, a: 1}).hint("a_1").toArray().length, 1);
// TODO SERVER-26413 using a hint causes a partial result to be returned
assert.lte(db.partial_filter_expression.find({_id: 2, a: 1}).hint("a_1").toArray().length, 1);

// partialFilterExpression on a subfield of _id

db.partial_filter_expression2.insertOne({a: 1});
db.partial_filter_expression2.createIndex({a: 1}, {partialFilterExpression: {"_id.a": 1}});

db.partial_filter_expression2.insertOne({_id: {a: 1}, a: 1});
db.partial_filter_expression2.insertOne({_id: {a: 2}, a: 1});
assert.eq(
    db.partial_filter_expression2
        .find({_id: {a: 1}, a: 1})
        .hint("a_1")
        .toArray(),
    [{_id: {a: 1}, a: 1}],
);

// createIndex() + wildcardProjection

// Index Key and Wildcard Projection cannot contain overlapping fields, however '_id' index field is overlapping with '_id' wildcardProjection path
assert.commandFailedWithCode(
    db.wildcard_projection1.createIndex({_id: 1, "$**": 1}, {wildcardProjection: {_id: 1}}),
    7246208,
);
assert.commandFailedWithCode(
    db.wildcard_projection1.createIndex({"_id.a": 1, "$**": 1}, {wildcardProjection: {"_id.a": 1}}),
    7246208,
);
assert.commandFailedWithCode(
    db.wildcard_projection1.createIndex({"$**": 1, _id: 1}, {wildcardProjection: {_id: 1}}),
    7246208,
);

// compound index that explicitly includes _id as a separate field
db.wildcard_projection2.insertOne({_id: 1, a: 1});
assert.commandWorked(
    db.wildcard_projection2.createIndex({_id: 1, "$**": 1}, {wildcardProjection: {_id: 0}}),
);
assert.eq(db.wildcard_projection2.find({_id: 1}).hint("_id_1_$**_1").toArray(), [{_id: 1, a: 1}]);

// wildcard index that explicitly includes _id in the wildcard field
db.wildcard_projection3.insertOne({_id: 1, a: 1});
assert.commandWorked(
    db.wildcard_projection3.createIndex({"$**": 1}, {wildcardProjection: {"_id": 1}}),
);
assert.eq(db.wildcard_projection3.find({_id: 1}).hint("$**_1").toArray(), [{_id: 1, a: 1}]);

// wildcard index that explicitly includes a subfield of _id
db.wildcard_projection4.insertOne({_id: {a: 1}, a: 1});
assert.commandWorked(
    db.wildcard_projection4.createIndex({"$**": 1}, {wildcardProjection: {"_id.a": 1}}),
);
assert.eq(db.wildcard_projection4.find({"_id.a": 1}).hint("$**_1").toArray(), [
    {_id: {a: 1}, a: 1},
]);

// createIndex() + collation

assert.commandWorked(db.createCollection("create_index_fr", {collation: {locale: "fr"}}));
assert.commandFailedWithCode(
    db.create_index_fr.createIndex({_id: 1}, {collation: {locale: "simple"}}),
    ErrorCodes.BadValue,
);
assert.commandFailedWithCode(
    db.create_index_fr.createIndex({_id: 1}, {collation: {locale: "fr", strength: 2}}),
    ErrorCodes.BadValue,
);
assert.commandFailedWithCode(
    db.create_index_fr.createIndex({_id: 1}, {collation: {locale: "fr", version: "123"}}),
    ErrorCodes.IncompatibleCollationVersion,
);

/*
 * ==============================
 * createIndexes()
 * ==============================
 */

db.create_indexes.insertOne({});

// Make sure that the fact that we are trying to mess with the _id index as the second index definition does not allow
// us to pass invalid options.
assert.commandFailedWithCode(
    db.create_indexes.createIndexes([{a: 1}, {_id: -1}]),
    ErrorCodes.BadValue,
);
assert.commandFailedWithCode(
    db.create_indexes.createIndexes([{a: 1}, {_id: 1}], {unique: true}),
    ErrorCodes.InvalidIndexSpecificationOption,
);
assert.commandFailedWithCode(
    db.create_indexes.createIndexes([{a: 1}, {_id: 1}], {sparse: true}),
    ErrorCodes.InvalidIndexSpecificationOption,
);
assert.commandFailedWithCode(
    db.create_indexes.createIndexes([{a: 1}, {_id: 1}], {expireAfterSeconds: 3600}),
    ErrorCodes.InvalidIndexSpecificationOption,
);
assert.commandFailedWithCode(
    db.create_indexes.createIndexes([{a: 1}, {_id: 1}], {hidden: true}),
    ErrorCodes.InvalidIndexSpecificationOption,
);
assert.commandFailedWithCode(
    db.create_indexes.createIndexes([{a: 1}, {_id: 1}], {partialFilterExpression: {_id: 1}}),
    ErrorCodes.InvalidIndexSpecificationOption,
);

assert.commandFailedWithCode(
    db.create_indexes.createIndexes([{a: 1}, {_id: 1}], {collation: {locale: "fr"}}),
    ErrorCodes.BadValue,
);

/*
 * ==============================
 * dropIndex()
 * ==============================
 */

db.drop_index.insertOne({});

assert.commandFailedWithCode(
    db.drop_index.dropIndex({_id: 1}),
    [ErrorCodes.InvalidOptions, 5979800], // It is illegal to drop the clusteredIndex
);
assert.commandFailedWithCode(
    db.drop_index.dropIndex("_id_"),
    [ErrorCodes.InvalidOptions, 5979800], // It is illegal to drop the clusteredIndex
);

// drop index on _id: -1 succeeds but has no effect
assert.commandWorked(db.drop_index.dropIndex({_id: -1}));
assert.gte(db.drop_index.getIndexes().length, 1);
db.drop_index.insertOne({_id: 1});

/*
 * ==============================
 * hideIndex()
 * ==============================
 */

db.hide_index.insertOne({});

assert.commandFailedWithCode(
    db.hide_index.hideIndex({_id: 1}),
    [ErrorCodes.BadValue, 6011800], // The 'hidden' option is not supported for a clusteredIndex
);
assert.commandFailedWithCode(
    db.hide_index.hideIndex("_id_"),
    [ErrorCodes.BadValue, 6011800], // The 'hidden' option is not supported for a clusteredIndex
);
