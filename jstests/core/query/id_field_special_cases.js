/**
 * This test was generated via an LLM that was prompted to find code paths that are special-cased for the _id field.
 *
 * Exercises server-side special handling of the reserved `_id` field across CRUD, aggregation,
 * indexing, and BSON validation paths (e.g. validIdField, immutable _id, $group _id, _id index).
 *
 * @tags: [
 *   # Time-series collections use a different _id discipline.
 *   exclude_from_timeseries_crud_passthrough,
 *   # Test uses commands that cannot be blindly retried.
 *   requires_non_retryable_commands,
 *   requires_non_retryable_writes,
 *   does_not_support_retryable_writes,
 *   # Test does not survive cluster disruptions
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 * ]
 */

db.dropDatabase();

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.id_field_special_cases;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName()));

// --- Insert: validIdField rejects regex, array, undefined; accepts common scalars and plain subdocs.
assert.writeErrorWithCode(coll.insert({_id: /x/, a: 1}), ErrorCodes.InvalidIdField);
assert.writeErrorWithCode(coll.insert({_id: [1, 2], a: 1}), [
    ErrorCodes.InvalidIdField,
    ErrorCodes.ShardKeyNotFound,
]);
assert.writeErrorWithCode(coll.insert({_id: undefined, a: 1}), ErrorCodes.InvalidIdField);
assert.commandWorked(coll.insert({_id: null, k: "null_id"}));
assert.commandWorked(coll.insert({_id: {x: 1, y: 2}, k: "obj_id"}));
assert.commandWorked(coll.insert({k: "generated"}));
const gen = coll.findOne({k: "generated"});
assert(gen._id != null && gen._id instanceof ObjectId, tojson(gen));

// --- Insert: _id subdocuments may not contain '$'-prefixed field names (storage validation).
assert.writeErrorWithCode(
    coll.insert({_id: {$a: 1}, k: "bad"}),
    ErrorCodes.DollarPrefixedFieldName,
);
assert.writeErrorWithCode(
    coll.insert({_id: {a: {$b: 1}}, k: "bad_nested"}),
    ErrorCodes.DollarPrefixedFieldName,
);

// --- Insert: duplicate `_id` keys in one BSON object (validated in insert path).
const dupIdBson = _buildBsonObj("_id", 1, "_id", 2);
assert.writeErrorWithCode(coll.insert(dupIdBson), ErrorCodes.BadValue);

// --- Update: _id is immutable for modifier updates and pipeline updates.
assert.commandWorked(coll.insert({_id: 0, k: "imm"}));
assert.commandFailedWithCode(coll.update({_id: 0}, {$set: {_id: 1}}), ErrorCodes.ImmutableField);
assert.commandFailedWithCode(
    testDb.runCommand({
        update: coll.getName(),
        updates: [{q: {_id: 0}, u: [{$set: {_id: 10}}]}],
    }),
    ErrorCodes.ImmutableField,
);

// --- Replacement update: cannot change _id to a different value.
assert.commandFailedWithCode(
    testDb.runCommand({
        update: coll.getName(),
        updates: [{q: {_id: 0}, u: {_id: 99, k: "replaced"}}],
    }),
    ErrorCodes.ImmutableField,
);

// --- Replacement update: omitting _id in the replacement preserves the existing _id (combine path).
assert.commandWorked(
    testDb.runCommand({
        update: coll.getName(),
        updates: [{q: {_id: 0}, u: {k: "imm", z: 1}}],
    }),
);
assert.eq(0, coll.findOne({k: "imm", z: 1})._id);

// --- Upsert: auto-generated _id when the filter does not fix `_id`.
coll.deleteMany({k: "upsert_id"});
assert.commandWorked(coll.update({k: "upsert_id"}, {$set: {v: 1}}, {upsert: true}));
const upserted = coll.findOne({k: "upsert_id"});
assert.neq(upserted._id, undefined);
assert.eq(1, upserted.v);

// --- findAndModify respects immutable _id.
assert.commandFailedWithCode(
    testDb.runCommand({
        findAndModify: coll.getName(),
        query: {_id: 0},
        update: {$set: {_id: 42}},
    }),
    ErrorCodes.ImmutableField,
);

// --- Query: equality and $eq on _id; object-valued _id exact match.
coll.deleteMany({});
assert.commandWorked(
    coll.insertMany([
        {_id: 1, t: "a"},
        {_id: {p: 1}, t: "b"},
    ]),
);
assert.eq("a", coll.findOne({_id: 1}).t);
assert.eq("a", coll.findOne({_id: {$eq: 1}}).t);
assert.eq("b", coll.findOne({_id: {p: 1}}).t);

// --- Projection: inclusion adds _id by default; explicit _id exclusion works.
let proj = coll.findOne({_id: 1}, {t: 1});
assert.eq("a", proj.t);
assert.eq(1, proj._id);
proj = coll.findOne({_id: 1}, {t: 1, _id: 0});
assert.eq("a", proj.t);
assert(!proj.hasOwnProperty("_id"), tojson(proj));

// --- Sort and distinct on _id.
const byIdAsc = coll.find({}).sort({_id: 1}).toArray();
assert.eq(2, byIdAsc.length);
assert.eq(1, byIdAsc[0]._id);
assert.docEq({p: 1}, byIdAsc[1]._id);
const distinctIds = coll.distinct("_id");
assert.sameMembers([1, {p: 1}], distinctIds, () => tojson(distinctIds));

// --- Aggregation: $group requires `_id`; grouping uses the reserved output field name `_id`.
assert.commandFailedWithCode(
    testDb.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$group: {c: {$sum: 1}}}],
        cursor: {},
    }),
    15955,
);
let ag = coll.aggregate([{$group: {_id: "$t", c: {$sum: 1}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(2, ag.length);
assert.eq("a", ag[0]._id);
assert.eq(1, ag[0].c);
assert.eq("b", ag[1]._id);
assert.eq(1, ag[1].c);

ag = coll.aggregate([{$group: {_id: {d: "$t"}, c: {$sum: 1}}}, {$sort: {"_id.d": 1}}]).toArray();
assert.eq(2, ag.length);
assert.eq("a", ag[0]._id.d);
assert.eq(1, ag[0].c);
assert.eq("b", ag[1]._id.d);
assert.eq(1, ag[1].c);

ag = coll.aggregate([{$group: {_id: null, c: {$sum: 1}}}]).toArray();
assert.eq(1, ag.length);
assert.eq(null, ag[0]._id);
assert.eq(2, ag[0].c);

// --- Second explicit `_id` index is rejected (catalog / validateIdIndexSpec).
assert.commandFailed(coll.createIndex({_id: 1}, {name: "other_name", collation: {locale: "fr"}}));

// --- `_id` index cannot be hidden.
assert.commandFailed(coll.hideIndex("_id_"));

// --- Hint on the default _id index is allowed for an _id query.
assert.commandWorked(
    testDb.runCommand({find: coll.getName(), filter: {_id: 1}, hint: {_id: 1}, limit: 1}),
);

coll.drop();
