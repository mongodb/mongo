/**
 * Tests that the $graphLookup stage respects the collation when matching between the
 * 'connectFromField' and the 'connectToField'.  $graphLookup should use the collation
 * set on the aggregation, or the default collation of the collection.
 */
(function() {
    "use strict";

    var res;
    const caseInsensitiveUS = {collation: {locale: "en_US", strength: 2}};
    const caseSensitiveUS = {collation: {locale: "en_US", strength: 3}};

    var coll = db.collation_graphlookup;
    var foreignColl = db.collation_graphlookup_foreign;

    // Test that $graphLookup respects the collation set on the aggregation pipeline. Case
    // insensitivity should mean that we find both "jeremy" and "jimmy" as friends.
    coll.drop();
    assert.writeOK(coll.insert({username: "erica", friends: ["jeremy", "jimmy"]}));
    assert.writeOK(coll.insert([{username: "JEREMY"}, {username: "JIMMY"}]));

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

    // Negative test: ensure that we don't find any friends when the collation is simple.
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

    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), caseInsensitiveUS));
    assert.writeOK(coll.insert({username: "erica", friends: ["jeremy", "jimmy"]}));
    foreignColl.drop();
    assert.commandWorked(db.createCollection(foreignColl.getName(), caseSensitiveUS));
    assert.writeOK(foreignColl.insert([{username: "JEREMY"}, {username: "JIMMY"}]));

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
    assert.writeOK(coll.insert({username: "erica", friends: ["jeremy"]}));
    assert.writeOK(coll.insert({_id: "foo", username: "JEREMY", friends: ["jimmy"]}));
    assert.writeOK(coll.insert({_id: "FOO", username: "jimmy", friends: []}));

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
    assert.writeOK(coll.insert({username: "erica", friends: ["jeremy"]}));
    assert.writeOK(coll.insert({_id: "foo", username: "jeremy"}));
    assert.writeOK(coll.insert({_id: "FOO", username: "JEREMY"}));

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
