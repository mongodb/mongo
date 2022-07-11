/**
 * Tests that the $graphLookup stage respects the collation when matching between the
 * 'connectFromField' and the 'connectToField'.  $graphLookup should use the collation
 * set on the aggregation, or the default collation of the collection.
 * @tags: [assumes_no_implicit_collection_creation_after_drop]
 */
(function() {
"use strict";

var res;
const caseInsensitiveUS = {
    collation: {locale: "en_US", strength: 2}
};
const caseSensitiveUS = {
    collation: {locale: "en_US", strength: 3}
};

var coll = db.collation_graphlookup;
var foreignColl = db.collation_graphlookup_foreign;

// Test that $graphLookup respects the collation set on the aggregation pipeline. Case
// insensitivity should mean that we find both "jeremy" and "jimmy" as friends.
coll.drop();
assert.commandWorked(coll.insert({username: "erica", friends: ["jeremy", "jimmy"]}));
assert.commandWorked(coll.insert([{username: "JEREMY"}, {username: "JIMMY"}]));

res = coll.aggregate(
                [
                    {$match: {username: "erica"}},
                    {
                    $graphLookup: {
                        from: coll.getName(),
                        startWith: "$friends",
                        connectFromField: "friends",
                        connectToField: "username",
                        as: "friendUsers"
                    }
                    }
                ],
                caseInsensitiveUS)
            .toArray();
assert.eq(1, res.length);
assert.eq("erica", res[0].username);
assert.eq(2, res[0].friendUsers.length);

// Negative test: The local collation does not have a default collation, and so we use the simple
// collation. Ensure that we don't find any friends when the collation is simple.
res = coll.aggregate([
                {$match: {username: "erica"}},
                {
                    $graphLookup: {
                        from: coll.getName(),
                        startWith: "$friends",
                        connectFromField: "friends",
                        connectToField: "username",
                        as: "friendUsers"
                    }
                }
            ])
            .toArray();
assert.eq(1, res.length);
assert.eq("erica", res[0].username);
assert.eq(0, res[0].friendUsers.length);

// Create a foreign collection with a case-insensitive default collation.
foreignColl.drop();
assert.commandWorked(db.createCollection(foreignColl.getName(), caseInsensitiveUS));
assert.commandWorked(foreignColl.insert([{username: "JEREMY"}, {username: "JIMMY"}]));

// Insert some more documents into the local collection.
assert.commandWorked(coll.insert({username: "fiona", friends: ["jeremy"]}));
assert.commandWorked(coll.insert({username: "gretchen", friends: ["jimmy"]}));

// Test that $graphLookup uses the simple collation in the case where the collection on which it is
// run does not have a default collation, and that this collation is used instead of the default
// collation of the foreign collection. Exercises the fix for SERVER-43350.
res = coll.aggregate([{$match: {username: {$in: ["erica", "fiona", "gretchen"]}}},
                {
                    $graphLookup: {
                        from: foreignColl.getName(),
                        startWith: "$friends",
                        connectFromField: "friends",
                        connectToField: "username",
                        as: "friendUsers"
                    }
                }
            ])
            .toArray();
assert.eq(3, res.length, tojson(res));
for (let i = 0; i < res.length; ++i) {
    assert(["erica", "fiona", "gretchen"].includes(res[i].username));
    assert.eq(0, res[i].friendUsers.length);
}

// Recreate the local collection with a case-insensitive default collation, and the foreign
// collection with a case-sensitive default collation.
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitiveUS));
assert.commandWorked(coll.insert({username: "erica", friends: ["jeremy", "jimmy"]}));
foreignColl.drop();
assert.commandWorked(db.createCollection(foreignColl.getName(), caseSensitiveUS));
assert.commandWorked(foreignColl.insert([{username: "JEREMY"}, {username: "JIMMY"}]));

// Test that $graphLookup inherits the default collation of the collection on which it is run,
// and that this collation is used instead of the default collation of the foreign collection.
res = coll.aggregate([
                {$match: {username: "erica"}},
                {
                    $graphLookup: {
                        from: foreignColl.getName(),
                        startWith: "$friends",
                        connectFromField: "friends",
                        connectToField: "username",
                        as: "friendUsers"
                    }
                }
            ])
            .toArray();
assert.eq(1, res.length);
assert.eq("erica", res[0].username);
assert.eq(2, res[0].friendUsers.length);

// Test that we don't use the collation to dedup string _id values. This would cause us to miss
// nodes in the graph that have distinct _id values which compare equal under the collation.
coll.drop();
assert.commandWorked(coll.insert({username: "erica", friends: ["jeremy"]}));
assert.commandWorked(coll.insert({_id: "foo", username: "JEREMY", friends: ["jimmy"]}));
assert.commandWorked(coll.insert({_id: "FOO", username: "jimmy", friends: []}));

res = coll.aggregate(
                [
                    {$match: {username: "erica"}},
                    {
                    $graphLookup: {
                        from: coll.getName(),
                        startWith: "$friends",
                        connectFromField: "friends",
                        connectToField: "username",
                        as: "friendUsers"
                    }
                    }
                ],
                caseInsensitiveUS)
            .toArray();
assert.eq(1, res.length);
assert.eq("erica", res[0].username);
assert.eq(2, res[0].friendUsers.length);

// Test that the result set is not deduplicated under the collation. If two documents are
// entirely equal under the collation, they should still both get returned in the "as" field.
coll.drop();
assert.commandWorked(coll.insert({username: "erica", friends: ["jeremy"]}));
assert.commandWorked(coll.insert({_id: "foo", username: "jeremy"}));
assert.commandWorked(coll.insert({_id: "FOO", username: "JEREMY"}));

res = coll.aggregate(
                [
                    {$match: {username: "erica"}},
                    {
                    $graphLookup: {
                        from: coll.getName(),
                        startWith: "$friends",
                        connectFromField: "friends",
                        connectToField: "username",
                        as: "friendUsers"
                    }
                    }
                ],
                caseInsensitiveUS)
            .toArray();
assert.eq(1, res.length);
assert.eq("erica", res[0].username);
assert.eq(2, res[0].friendUsers.length);
})();
