// Test $or with predicates that generate inexact bounds. The access planner
// has special logic for such queries.

var t = db.jstests_or_inexact;
var cursor;

// A predicate which uses an index falls into one of three categories:
//
// 1) EXACT
//    The predicate can be fully evaluated by the index bounds.
// 2) INEXACT_COVERED
//    The predicate cannot be fully evaluated by the index bounds. However,
//    there is enough information in the index key to evaluate. Such predicates
//    are answered by an index scan with an additional filter on the index key.
// 3) INEXACT_FETCH
//    We must fetch the full documents in order to evaluate the predicate.

// Case 1: An EXACT predicate and an INEXACT_COVERED
t.drop();
t.ensureIndex({name: 1});
t.insert({_id: 0, name: "thomas"});
t.insert({_id: 1, name: "alexandra"});
cursor = t.find({$or: [{name: "thomas"}, {name: /^alexand(er|ra)/}]});
assert.eq(2, cursor.itcount(), "case 1");

// Case 2: Two INEXACT_COVERED predicates.
t.drop();
t.ensureIndex({name: 1});
t.insert({_id: 0, name: "thomas"});
t.insert({_id: 1, name: "alexandra"});
cursor = t.find({$or: [{name: /omas/}, {name: /^alexand(er|ra)/}]});
assert.eq(2, cursor.itcount(), "case 2");

// Case 3: An EXACT, and INEXACT_COVERED, and an INEXACT_FETCH.
t.drop();
t.ensureIndex({names: 1});
t.insert({_id: 0, names: ["thomas", "alexandra"]});
t.insert({_id: 1, names: "frank"});
t.insert({_id: 2, names: "alice"});
t.insert({_id: 3, names: ["dave"]});
cursor = t.find(
    {$or: [{names: "frank"}, {names: /^al(ice|ex)/}, {names: {$elemMatch: {$eq: "thomas"}}}]});
assert.eq(3, cursor.itcount(), "case 3");

// Case 4: Two INEXACT_FETCH.
t.drop();
t.ensureIndex({names: 1});
t.insert({_id: 0, names: ["thomas", "alexandra"]});
t.insert({_id: 1, names: ["frank", "alice"]});
t.insert({_id: 2, names: "frank"});
cursor = t.find(
    {$or: [{names: {$elemMatch: {$eq: "alexandra"}}}, {names: {$elemMatch: {$eq: "frank"}}}]});
assert.eq(2, cursor.itcount(), "case 4");

// Case 5: Two indices. One has EXACT and INEXACT_COVERED. The other
// has EXACT and INEXACT_FETCH.
t.drop();
t.ensureIndex({first: 1});
t.ensureIndex({last: 1});
t.insert({_id: 0, first: "frank", last: "smith"});
t.insert({_id: 1, first: "john", last: "doe"});
t.insert({_id: 2, first: "dave", last: "st"});
t.insert({_id: 3, first: ["dave", "david"], last: "pasette"});
t.insert({_id: 4, first: "joanna", last: ["smith", "doe"]});
cursor = t.find(
    {$or: [{first: "frank"}, {last: {$elemMatch: {$eq: "doe"}}}, {first: /david/}, {last: "st"}]});
assert.eq(4, cursor.itcount(), "case 5");

// Case 6: Multikey with only EXACT predicates.
t.drop();
t.ensureIndex({names: 1});
t.insert({_id: 0, names: ["david", "dave"]});
t.insert({_id: 1, names: ["joseph", "joe", "joey"]});
cursor = t.find({$or: [{names: "dave"}, {names: "joe"}]});
assert.eq(2, cursor.itcount(), "case 6");

// Case 7: Multikey with EXACT and INEXACT_COVERED.
t.drop();
t.ensureIndex({names: 1});
t.insert({_id: 0, names: ["david", "dave"]});
t.insert({_id: 1, names: ["joseph", "joe", "joey"]});
cursor = t.find({$or: [{names: "dave"}, {names: /joe/}]});
assert.eq(2, cursor.itcount(), "case 7");

// Case 8: Text with EXACT.
t.drop();
t.ensureIndex({pre: 1, names: "text"});
t.ensureIndex({other: 1});
t.insert({_id: 0, pre: 3, names: "david dave", other: 1});
t.insert({_id: 1, pre: 4, names: "joseph joe joey", other: 2});
cursor = t.find({$or: [{$text: {$search: "dave"}, pre: 3}, {other: 2}]});
assert.eq(2, cursor.itcount(), "case 8");

// Case 9: Text with INEXACT_COVERED.
t.drop();
t.ensureIndex({pre: 1, names: "text"});
t.ensureIndex({other: 1});
t.insert({_id: 0, pre: 3, names: "david dave", other: "foo"});
t.insert({_id: 1, pre: 5, names: "david dave", other: "foo"});
t.insert({_id: 2, pre: 4, names: "joseph joe joey", other: "bar"});
cursor = t.find({$or: [{$text: {$search: "dave"}, pre: 3}, {other: /bar/}]});
assert.eq(2, cursor.itcount(), "case 9");

// Case 10: Text requiring filter with INEXACT_COVERED.
t.drop();
t.ensureIndex({pre: 1, names: "text"});
t.ensureIndex({other: 1});
t.insert({_id: 0, pre: 3, names: "david dave", other: "foo"});
t.insert({_id: 1, pre: 3, names: "david dave", other: "foo"});
t.insert({_id: 2, pre: 4, names: "joseph joe joey", other: "bar"});
cursor = t.find({$or: [{$text: {$search: "dave"}, pre: 3, other: "foo"}, {other: /bar/}]});
assert.eq(3, cursor.itcount(), "case 10");

// Case 11: GEO with non-geo, same index, 2dsphere.
t.drop();
t.ensureIndex({pre: 1, loc: "2dsphere"});
t.insert({_id: 0, pre: 3, loc: {type: "Point", coordinates: [40, 5]}});
t.insert({_id: 1, pre: 4, loc: {type: "Point", coordinates: [0, 0]}});
cursor = t.find({
    $or: [
        {
          pre: 3,
          loc: {
              $geoWithin: {
                  $geometry: {
                      type: "Polygon",
                      coordinates: [[[39, 4], [41, 4], [41, 6], [39, 6], [39, 4]]]
                  }
              }
          }
        },
        {
          pre: 4,
          loc: {
              $geoWithin: {
                  $geometry: {
                      type: "Polygon",
                      coordinates: [[[-1, -1], [1, -1], [1, 1], [-1, 1], [-1, -1]]]
                  }
              }
          }
        }
    ]
});
assert.eq(2, cursor.itcount(), "case 11");

// Case 12: GEO with non-geo, same index, 2d.
t.drop();
t.ensureIndex({pre: 1, loc: "2d"});
t.insert({_id: 0, pre: 3, loc: {type: "Point", coordinates: [40, 5]}});
t.insert({_id: 1, pre: 4, loc: {type: "Point", coordinates: [0, 0]}});
cursor = t.find({
    $or: [
        {
          pre: 3,
          loc: {
              $geoWithin: {
                  $geometry: {
                      type: "Polygon",
                      coordinates: [[[39, 4], [41, 4], [41, 6], [39, 6], [39, 4]]]
                  }
              }
          }
        },
        {
          pre: 4,
          loc: {
              $geoWithin: {
                  $geometry: {
                      type: "Polygon",
                      coordinates: [[[-1, -1], [1, -1], [1, 1], [-1, 1], [-1, -1]]]
                  }
              }
          }
        }
    ]
});
assert.eq(2, cursor.itcount(), "case 12");

// Case 13: $elemMatch object.
t.drop();
t.ensureIndex({"a.b": 1});
t.insert({_id: 0, a: [{b: 1}, {b: 2}]});
t.insert({_id: 1, a: [{b: 3}, {b: 4}]});
cursor = t.find({$or: [{a: {$elemMatch: {b: {$lte: 1}}}}, {a: {$elemMatch: {b: {$gte: 4}}}}]});
assert.eq(2, cursor.itcount(), "case 13");

// Case 14: $elemMatch object, below an AND.
t.drop();
t.ensureIndex({"a.b": 1});
t.insert({_id: 0, a: [{b: 1}, {b: 2}]});
t.insert({_id: 1, a: [{b: 2}, {b: 4}]});
cursor =
    t.find({"a.b": 2, $or: [{a: {$elemMatch: {b: {$lte: 1}}}}, {a: {$elemMatch: {b: {$gte: 4}}}}]});
assert.eq(2, cursor.itcount(), "case 14");

// Case 15: $or below $elemMatch.
t.drop();
t.ensureIndex({"a.b": 1});
t.insert({_id: 0, a: [{b: 1}, {b: 2}]});
t.insert({_id: 1, a: [{b: 2}, {b: 4}]});
t.insert({_id: 2, a: {b: 4}});
cursor = t.find({a: {$elemMatch: {$or: [{b: 1}, {b: 4}]}}});
assert.eq(2, cursor.itcount(), "case 15");

// Case 16: $or below $elemMatch with INEXACT_COVERED.
t.drop();
t.ensureIndex({"a.b": 1});
t.insert({_id: 0, a: [{b: "x"}, {b: "y"}]});
t.insert({_id: 1, a: [{b: "y"}, {b: ["y", "z"]}]});
t.insert({_id: 2, a: {b: ["y", "z"]}});
cursor = t.find({a: {$elemMatch: {$or: [{b: "x"}, {b: /z/}]}}});
assert.eq(2, cursor.itcount(), "case 16");

// Case 17: case from SERVER-14030.
t.drop();
t.ensureIndex({number: 1});
t.insert({number: null, user_id: 1});
t.insert({number: 2, user_id: 1});
t.insert({number: 1, user_id: 1});
cursor = t.find({$or: [{number: null}, {number: {$lte: 2}}]});
assert.eq(3, cursor.itcount(), "case 17");

// Case 18: $in with EXACT and INEXACT_COVERED.
t.drop();
t.ensureIndex({name: 1});
t.insert({_id: 0, name: "thomas"});
t.insert({_id: 1, name: "alexandra"});
cursor = t.find({name: {$in: ["thomas", /^alexand(er|ra)/]}});
assert.eq(2, cursor.itcount(), "case 18");

// Case 19: $in with EXACT, INEXACT_COVERED, and INEXACT_FETCH.
t.drop();
t.ensureIndex({name: 1});
t.insert({_id: 0, name: "thomas"});
t.insert({_id: 1, name: "alexandra"});
t.insert({_id: 2});
cursor = t.find({$or: [{name: {$in: ["thomas", /^alexand(er|ra)/]}}, {name: {$exists: false}}]});
assert.eq(3, cursor.itcount(), "case 19");

// Case 20: $in with EXACT, INEXACT_COVERED, and INEXACT_FETCH, two indices.
t.drop();
t.ensureIndex({a: 1});
t.ensureIndex({b: 1});
t.insert({_id: 0, a: "x", b: "y"});
t.insert({_id: 1, a: "z", b: "z"});
t.insert({_id: 2});
t.insert({_id: 3, a: "w", b: "x"});
t.insert({_id: 4, a: "l", b: "p"});
cursor =
    t.find({$or: [{a: {$in: [/z/, /x/]}}, {a: "w"}, {b: {$exists: false}}, {b: {$in: ["p"]}}]});
assert.eq(5, cursor.itcount(), "case 19");

// Case 21: two $geoWithin that collapse to a single GEO index scan.
t.drop();
t.ensureIndex({loc: "2dsphere"});
t.insert({_id: 0, loc: {type: "Point", coordinates: [40, 5]}});
t.insert({_id: 1, loc: {type: "Point", coordinates: [0, 0]}});
cursor = t.find({
    $or: [
        {
          loc: {
              $geoWithin: {
                  $geometry: {
                      type: "Polygon",
                      coordinates: [[[39, 4], [41, 4], [41, 6], [39, 6], [39, 4]]]
                  }
              }
          }
        },
        {
          loc: {
              $geoWithin: {
                  $geometry: {
                      type: "Polygon",
                      coordinates: [[[-1, -1], [1, -1], [1, 1], [-1, 1], [-1, -1]]]
                  }
              }
          }
        }
    ]
});
assert.eq(2, cursor.itcount(), "case 21");
