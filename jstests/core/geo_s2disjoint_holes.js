// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
//   assumes_no_implicit_index_creation,
// ]

//
// We should prohibit polygons with holes not bounded by their exterior shells.
//
// From spec:
//
// "For Polygons with multiple rings, the first must be the exterior ring and
// any others must be interior rings or holes."
// http://geojson.org/geojson-spec.html#polygon
//

(function() {
'use strict';

const coordinates = [
    // One square.
    [[9, 9], [9, 11], [11, 11], [11, 9], [9, 9]],
    // Another disjoint square.
    [[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]
];

const poly = {
    type: 'Polygon',
    coordinates: coordinates
},
      multiPoly = {
          type: 'MultiPolygon',
          // Multi-polygon's coordinates are wrapped in one more array.
          coordinates: [coordinates]
      };

const collNamePrefix = 'geo_s2disjoint_holes_';
let collCount = 0;
let t = db.getCollection(collNamePrefix + collCount++);
t.drop();

jsTest.log("We're going to print some error messages, don't be alarmed.");

//
// Can't query with a polygon or multi-polygon that has a non-contained hole.
//
print(assert.throws(function() {
          t.findOne({geo: {$geoWithin: {$geometry: poly}}});
      }, [], "parsing a polygon with non-overlapping holes."));

print(assert.throws(function() {
          t.findOne({geo: {$geoWithin: {$geometry: multiPoly}}});
      }, [], "parsing a multi-polygon with non-overlapping holes."));

//
// Can't insert a bad polygon or a bad multi-polygon with a 2dsphere index.
//
assert.commandWorked(t.createIndex({p: '2dsphere'}));
assert.writeError(t.insert({p: poly}));
assert.writeError(t.insert({p: multiPoly}));

//
// Can't create a 2dsphere index when the collection contains a bad polygon or
// bad multi-polygon.
//
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.insert({p: poly});
assert.commandFailedWithCode(t.createIndex({p: '2dsphere'}), 16755);

t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.insert({p: multiPoly});
assert.commandFailedWithCode(t.createIndex({p: '2dsphere'}), 16755);

//
// But with no index we can insert bad polygons and bad multi-polygons.
//
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.insert({p: poly}));
assert.commandWorked(t.insert({p: multiPoly}));

jsTest.log("Success.");
})();
