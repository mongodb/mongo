/**
 * Deterministic repro for the Express write path crash that fires when the query
 * filter is shaped {_id: {$eq: <object>}} and the wrapped object's first field
 * name starts with '$' (the canonical example being a DBRef literal,
 * {$ref: "foo", $id: <oid>}).
 *
 * Background:
 *   - The Express write path is enabled by isSimpleIdQuery(), which permits
 *     {_id: {$eq: <value>}} as an exact-_id query.
 *   - On entry, makeExpressExecutorForUpdate / makeExpressExecutorForDelete
 *     call getQueryFilterMaybeUnwrapEq() to flatten {_id: {$eq: <v>}} into
 *     {_id: <v>}.
 *   - The unwrapped filter is then re-validated by isExactMatchOnId(), which
 *     rejects object values whose first sub-field begins with '$' on the
 *     assumption that such a sub-field is an operator. A DBRef literal looks
 *     exactly like that, but it is data, not an operator.
 *   - The mismatch trips tassert(9248801) for update and tassert(9248804) for
 *     delete, taking mongod down.
 *
 * This test pins the user-visible contract: an update or delete with such a
 * filter must not crash. The server is permitted to either succeed with an
 * empty match, succeed with a match on a document whose _id is the equivalent
 * DBRef literal, or fail with a structured command error. What it must not do
 * is tassert.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   assumes_write_concern_unchanged,
 *   requires_non_retryable_writes,
 *   # Time-series collections have different _id properties and do not exercise
 *   # the Express _id-equality fast path.
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */

const coll = db.getCollection("express_id_eq_dbref_crash");
coll.drop();

// Seed with a normal _id document so the collection is non-empty and the
// Express path has a real index entry to compare against.
assert.commandWorked(coll.insert({_id: 1, x: 0}));

// Build the offending filter once. The literal DBRef shape is what trips the
// Express re-validation: its first sub-field ('$ref') starts with '$' even
// though it is plain data.
const dbrefLiteral = {
    $ref: "foo",
    $id: 1,
};
const offendingFilter = {
    _id: {$eq: dbrefLiteral},
};

// ---- Update path ----------------------------------------------------------
// Pre-bug, this would tassert(9248801) and tear mongod down. Post-fix the
// command may return a clean success with nMatched == 0, or a structured
// write error. We accept either, but the server must remain alive.
const updateRes = db.runCommand({
    update: coll.getName(),
    updates: [
        {q: offendingFilter, u: {$set: {x: 1}}, multi: false, upsert: false},
    ],
});

// If the command-level call itself returned ok:0, that is acceptable as long
// as the structured response is well-formed (no crash). Inspect writeErrors
// without asserting on a particular error code: the contract here is that the
// server returns a response, not that it picks any specific shape.
assert(typeof updateRes === "object" && updateRes !== null,
       "update returned a non-object response: " + tojson(updateRes));

if (updateRes.ok === 1) {
    // Either nothing matched (DBRef literal does not equal _id: 1), or the
    // server matched and modified a doc whose _id is a DBRef literal. Both
    // are allowed; we only require nMatched to be a non-negative integer.
    assert.eq(typeof updateRes.n, "number",
              "update missing numeric n: " + tojson(updateRes));
    assert.gte(updateRes.n, 0,
               "update reported negative n: " + tojson(updateRes));
} else {
    // ok:0 path: expect a writeErrors array OR a top-level errmsg/code.
    const hasStructuredError =
        (Array.isArray(updateRes.writeErrors) && updateRes.writeErrors.length > 0) ||
        (typeof updateRes.errmsg === "string" && typeof updateRes.code === "number");
    assert(hasStructuredError,
           "update returned ok:0 without a structured error: " + tojson(updateRes));
}

// The seed document must still be reachable after the update attempt. If the
// server crashed earlier, this read would not return. If the update silently
// corrupted state, the seed would be missing.
assert.eq(coll.find({_id: 1}).itcount(), 1,
          "seed document {_id: 1} disappeared after update attempt");

// ---- Delete path ----------------------------------------------------------
// Symmetric repro for tassert(9248804). Same allowance: clean success with
// zero matches, clean success deleting a DBRef-_id document, or a structured
// write error. No crash.
const deleteRes = db.runCommand({
    delete: coll.getName(),
    deletes: [
        {q: offendingFilter, limit: 1},
    ],
});

assert(typeof deleteRes === "object" && deleteRes !== null,
       "delete returned a non-object response: " + tojson(deleteRes));

if (deleteRes.ok === 1) {
    assert.eq(typeof deleteRes.n, "number",
              "delete missing numeric n: " + tojson(deleteRes));
    assert.gte(deleteRes.n, 0,
               "delete reported negative n: " + tojson(deleteRes));
} else {
    const hasStructuredError =
        (Array.isArray(deleteRes.writeErrors) && deleteRes.writeErrors.length > 0) ||
        (typeof deleteRes.errmsg === "string" && typeof deleteRes.code === "number");
    assert(hasStructuredError,
           "delete returned ok:0 without a structured error: " + tojson(deleteRes));
}

// Server liveness post-delete. A follow-up find round-trip is the cheapest
// proof that the connection survived both write attempts.
assert.eq(coll.find({_id: 1}).itcount(), 1,
          "seed document {_id: 1} disappeared after delete attempt");

// ---- findAndModify spot check --------------------------------------------
// findAndModify also routes through the Express write path for exact-_id
// updates/deletes. Cover both modes briefly. A crash here would manifest as
// a missing response; correctness for the remove/update result itself is not
// the point of this test, so we only require a well-formed command response.
const famUpdateRes = db.runCommand({
    findAndModify: coll.getName(),
    query: offendingFilter,
    update: {$set: {x: 2}},
});
assert(typeof famUpdateRes === "object" && famUpdateRes !== null,
       "findAndModify(update) returned a non-object response: " + tojson(famUpdateRes));

const famRemoveRes = db.runCommand({
    findAndModify: coll.getName(),
    query: offendingFilter,
    remove: true,
});
assert(typeof famRemoveRes === "object" && famRemoveRes !== null,
       "findAndModify(remove) returned a non-object response: " + tojson(famRemoveRes));

// Final liveness probe.
assert.eq(coll.find({_id: 1}).itcount(), 1,
          "seed document {_id: 1} disappeared after findAndModify attempts");
