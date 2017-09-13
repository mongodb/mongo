/**
 * Tests that a user can group on the text score.
 */
(function() {
    "use strict";
    const coll = db.text_score_grouping;

    coll.drop();

    assert.writeOK(coll.insert({"_id": 1, "title": "cakes"}));
    assert.writeOK(coll.insert({"_id": 2, "title": "cookies and cakes"}));

    assert.commandWorked(coll.createIndex({title: "text"}));

    // Make sure there are two distinct groups for a text search with no other dependencies.
    var results = coll.aggregate([
                          {$match: {$text: {$search: "cake cookies"}}},
                          {$group: {_id: {$meta: "textScore"}, count: {$sum: 1}}}
                      ])
                      .toArray();
    assert.eq(results.length, 2);

    // Make sure there are two distinct groups if there are other fields required by the group.
    results = coll.aggregate([
                      {$match: {$text: {$search: "cake cookies"}}},
                      {$group: {_id: {$meta: "textScore"}, firstId: {$first: "$_id"}}}
                  ])
                  .toArray();
    assert.eq(results.length, 2);

}());
