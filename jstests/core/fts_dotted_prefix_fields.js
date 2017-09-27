// Test that text search works correct when the text index has dotted paths as the non-text
// prefixes.
(function() {
    "use strict";

    let coll = db.fts_dotted_prefix_fields;
    coll.drop();
    assert.commandWorked(coll.createIndex({"a.x": 1, "a.y": 1, "b.x": 1, "b.y": 1, words: "text"}));
    assert.writeOK(coll.insert({a: {x: 1, y: 2}, b: {x: 3, y: 4}, words: "lorem ipsum dolor sit"}));
    assert.writeOK(coll.insert({a: {x: 1, y: 2}, b: {x: 5, y: 4}, words: "lorem ipsum dolor sit"}));

    assert.eq(1,
              coll.find({$text: {$search: "lorem ipsum"}, "a.x": 1, "a.y": 2, "b.x": 3, "b.y": 4})
                  .itcount());
}());
