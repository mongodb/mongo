/**
 * SERVER-124369 regression — replacement-style update documents that carry
 * reserved oplog-update field names (e.g. $diff, $set, $v) as ordinary
 * keys must be applied LITERALLY on the secondary, not misparsed as a
 * delta update.
 *
 * Background:
 *   Replication classifies an update oplog entry by looking at the shape
 *   of `o`. A v2 delta update has the form `{$v: 2, diff: {...}}` and is
 *   routed through `write_ops::UpdateModification::parseFromOplogEntry`
 *   (src/mongo/db/exec/agg/internal_apply_oplog_update_stage.cpp). A
 *   replacement-style update is `o = <full new document>`, with NO
 *   wrapping $v / diff. The bug class: when the user's *replacement*
 *   payload happens to contain a top-level field literally named `$diff`
 *   (or another reserved oplog token), the classifier latches onto the
 *   reserved name and tries to apply the value as a delta — corrupting
 *   the document or asserting on the secondary.
 *
 * This test pins the contract: for every reserved token T in the
 * oplog-update vocabulary, a replacement `{T: <value>, ...}` issued on
 * the primary must be visible on the secondary byte-for-byte, and the
 * recorded oplog entry must be the replacement form (no $v: 2 wrapper).
 *
 * Do NOT promote any of these tokens to "interpreted" semantics without
 * also bumping the oplog version sentinel — this test will fail loudly.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({nodes: 2, oplogSize: 4});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();
const dbName = "server124369";
const collName = "reserved_field_names";
const primaryColl = primary.getDB(dbName)[collName];
const secondaryColl = secondary.getDB(dbName)[collName];
const oplog = primary.getDB("local").oplog.rs;

// Reserved tokens that appear in v2-delta oplog entries or are otherwise
// part of the update-modifier vocabulary. Each must round-trip as DATA
// when it appears in a replacement update.
const RESERVED_TOKENS = [
    "$diff",   // v2 delta wrapper key (the SERVER-124369 trigger)
    "$set",    // classic update modifier
    "$unset",  // classic update modifier
    "$v",      // oplog version sentinel
    "$rename", // classic update modifier
    "$inc",    // classic update modifier
];

// One representative value per token. We deliberately pick values that
// would be syntactically valid as the right-hand side of the modifier,
// so a buggy parser would happily try to apply them.
const RESERVED_VALUES = {
    "$diff": {u: {a: 99}, i: {b: "should-not-apply"}},
    "$set": {a: 99},
    "$unset": {a: ""},
    "$v": 2,
    "$rename": {a: "b"},
    "$inc": {a: 1},
};

function getLastUpdateOplogEntry(ns) {
    return oplog
        .find({op: "u", ns: ns})
        .sort({$natural: -1})
        .limit(1)
        .next();
}

function assertReplicated(doc, msg) {
    replTest.awaitReplication();
    const onSecondary = secondaryColl.findOne({_id: doc._id});
    assert.docEq(doc, onSecondary,
                 "secondary diverged from primary for " + msg);
}

function runOne(token, idx) {
    const _id = "rf-" + idx;
    const seed = {_id: _id, anchor: "before-replace"};
    assert.commandWorked(primaryColl.insert(seed));
    assertReplicated(seed, "seed-insert for " + token);

    // Build a replacement document whose top-level fields include the
    // reserved token. The replace() path issues a replacement-style
    // update — NOT a $set / delta — so the oplog `o` should be the
    // literal replacement body.
    const replacement = {_id: _id, anchor: "after-replace", marker: token};
    replacement[token] = RESERVED_VALUES[token];

    const res = assert.commandWorked(primaryColl.replaceOne({_id: _id}, replacement));
    assert.eq(res.modifiedCount, 1, "replaceOne did not modify for " + token);

    // 1. Primary must reflect the replacement literally.
    const onPrimary = primaryColl.findOne({_id: _id});
    assert.docEq(replacement, onPrimary,
                 "primary lost reserved field " + token);

    // 2. The oplog entry must be the replacement shape, not v2 delta.
    const entry = getLastUpdateOplogEntry(primaryColl.getFullName());
    assert(entry, "no update oplog entry recorded for " + token);
    assert(!entry.o.hasOwnProperty("$v") || entry.o.$v !== 2,
           "expected replacement-style oplog entry for " + token +
           " but got delta: " + tojson(entry));
    assert.docEq(replacement, entry.o,
                 "oplog `o` does not match replacement for " + token);

    // 3. Secondary must converge bit-identically. This is the real
    //    regression assertion — a misclassifying parser corrupts here.
    assertReplicated(replacement, "post-replace for " + token);
}

RESERVED_TOKENS.forEach(runOne);

// Nested coverage: the reserved token appears as a *value-side* key
// inside a non-reserved field. This must also be preserved literally
// — the classifier should only inspect top-level keys of `o`.
const nestedId = "rf-nested";
assert.commandWorked(primaryColl.insert({_id: nestedId, anchor: "before"}));
const nestedReplacement = {
    _id: nestedId,
    anchor: "after",
    payload: {"$diff": {u: {x: 1}}, "$v": 2, normal: "ok"},
};
assert.commandWorked(primaryColl.replaceOne({_id: nestedId}, nestedReplacement));
assert.docEq(nestedReplacement, primaryColl.findOne({_id: nestedId}),
             "primary lost nested reserved tokens");
assertReplicated(nestedReplacement, "nested reserved tokens");

// Combined coverage: a single replacement carrying every reserved token
// at once. Future-proofs against any classifier that short-circuits on
// the first reserved key seen.
const allId = "rf-all";
assert.commandWorked(primaryColl.insert({_id: allId, anchor: "before"}));
const allReplacement = {_id: allId, anchor: "after"};
RESERVED_TOKENS.forEach((t) => {
    allReplacement[t] = RESERVED_VALUES[t];
});
assert.commandWorked(primaryColl.replaceOne({_id: allId}, allReplacement));
assert.docEq(allReplacement, primaryColl.findOne({_id: allId}),
             "primary lost combined reserved tokens");
const allEntry = getLastUpdateOplogEntry(primaryColl.getFullName());
assert(!allEntry.o.hasOwnProperty("$v") || allEntry.o.$v !== 2,
       "combined replacement misclassified as delta: " + tojson(allEntry));
assertReplicated(allReplacement, "combined reserved tokens");

replTest.stopSet();
