// Integration tests for {$caseSensitive: true} option to $text query operator.

import {queryIDS} from "jstests/libs/fts.js";

let coll = db.fts_casesensitive;

coll.drop();

assert.commandWorked(coll.insert({_id: 0, a: "The Quick Brown Fox Jumps Over The Lazy Dog"}));
assert.commandWorked(coll.createIndex({a: "text"}));

assert.throws(function () {
    queryIDS(coll, "hello", null, {$caseSensitive: "invalid"});
});

assert.eq([0], queryIDS(coll, "The quick Brown", null, {$caseSensitive: true}));
assert.eq([0], queryIDS(coll, "Jumped", null, {$caseSensitive: true}));
assert.eq([0], queryIDS(coll, '"Quick"', null, {$caseSensitive: true}));
assert.eq([0], queryIDS(coll, '"Fox" Jumped', null, {$caseSensitive: true}));
assert.eq([0], queryIDS(coll, '"Fox Jumps" "Over The"', null, {$caseSensitive: true}));
assert.eq([0], queryIDS(coll, '"Fox Jumps" -"over the"', null, {$caseSensitive: true}));

assert.eq([], queryIDS(coll, "The", null, {$caseSensitive: true}));
assert.eq([], queryIDS(coll, "quick", null, {$caseSensitive: true}));
assert.eq([], queryIDS(coll, "The quick brown", null, {$caseSensitive: true}));
assert.eq([], queryIDS(coll, "The -quick -brown", null, {$caseSensitive: true}));
assert.eq([], queryIDS(coll, "The quick -brown", null, {$caseSensitive: true}));
assert.eq([], queryIDS(coll, "he Quic", null, {$caseSensitive: true}));
assert.eq([], queryIDS(coll, '"over the"', null, {$caseSensitive: true}));
assert.eq([], queryIDS(coll, '"Fox Jumps" -"Over The"', null, {$caseSensitive: true}));
