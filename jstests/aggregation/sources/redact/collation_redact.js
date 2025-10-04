// Cannot implicitly shard accessed collections because of collection existing when none expected.
// @tags: [
//   assumes_no_implicit_collection_creation_after_drop,
// ]

// Test that the $redact stage respects the collation.
let caseInsensitive = {collation: {locale: "en_US", strength: 2}};

let coll = db.collation_redact;
coll.drop();
assert.commandWorked(coll.insert({a: "a"}));

// Test that $redact respects an explicit collation. Since the top-level of the document gets
// pruned, we end up redacting the entire document and returning no results.
assert.eq(0, coll.aggregate([{$redact: {$cond: [{$eq: ["A", "a"]}, "$$PRUNE", "$$KEEP"]}}], caseInsensitive).itcount());

coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert({a: "a"}));

// Test that $redact respects the inherited collation. Since the top-level of the document gets
// pruned, we end up redacting the entire document and returning no results.
assert.eq(0, coll.aggregate([{$redact: {$cond: [{$eq: ["A", "a"]}, "$$PRUNE", "$$KEEP"]}}]).itcount());

// Test that a $match which can be optimized to be pushed before the $redact respects the
// collation.
assert.eq(1, coll.aggregate([{$redact: "$$KEEP"}, {$match: {a: "A"}}]).itcount());

// Comparison to the internal constants bound to the $$KEEP, $$PRUNE, and $$DESCEND variable
// should not respect the collation.
assert.throws(() => coll.aggregate([{$redact: "KEEP"}], caseInsensitive));
assert.throws(() => coll.aggregate([{$redact: "PRUNE"}], caseInsensitive));
assert.throws(() => coll.aggregate([{$redact: "REDACT"}], caseInsensitive));
