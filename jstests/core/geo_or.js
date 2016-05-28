// multiple geo clauses with $or

t = db.geoor;

t.drop();

var p = [-71.34895, 42.46037];
var q = [1.48736, 42.55327];

t.save({loc: p});
t.save({loc: q});

var indexname = "2dsphere";

t.ensureIndex({loc: indexname});

assert.eq(1, t.find({loc: p}).itcount(), indexname);

// $or supports at most one $near clause
assert.eq(2,
          t.find({$or: [{loc: {$nearSphere: p}}]}).itcount(),
          'geo query not supported by $or. index type: ' + indexname);
assert.throws(function() {
    assert.eq(2,
              t.find({$or: [{loc: {$nearSphere: p}}, {loc: {$nearSphere: q}}]}).itcount(),
              'geo query not supported by $or. index type: ' + indexname);
}, null, '$or with multiple $near clauses');

// the following tests should match the points in the collection

assert.eq(2,
          t.find({
               $or: [
                   {loc: {$geoWithin: {$centerSphere: [p, 10]}}},
                   {loc: {$geoWithin: {$centerSphere: [p, 10]}}}
               ]
           }).itcount(),
          'multiple $geoWithin clauses not supported by $or. index type: ' + indexname);
assert.eq(
    2,
    t.find({
         $or: [
             {loc: {$geoIntersects: {$geometry: {type: 'LineString', coordinates: [p, q]}}}},
             {
               loc: {
                   $geoIntersects:
                       {$geometry: {type: 'LineString', coordinates: [[0, 0], [1, 1]]}}
               }
             }
         ]
     }).itcount(),
    'multiple $geoIntersects LineString clauses not supported by $or. index type: ' + indexname);
assert.eq(2,
          t.find({
               $or: [
                   {loc: {$geoIntersects: {$geometry: {type: 'Point', coordinates: p}}}},
                   {loc: {$geoIntersects: {$geometry: {type: 'Point', coordinates: q}}}}
               ]
           }).itcount(),
          'multiple $geoIntersects Point clauses not supported by $or. index type: ' + indexname);
assert.eq(
    2,
    t.find({
         $or: [
             {
               loc: {
                   $geoIntersects:
                       {$geometry: {type: 'Polygon', coordinates: [[[0, 0], p, q, [0, 0]]]}}
               }
             },
             {
               loc: {
                   $geoIntersects: {
                       $geometry:
                           {type: 'Polygon', coordinates: [[[0, 0], [1, 1], [0, 1], [0, 0]]]}
                   }
               }
             }
         ]
     }).itcount(),
    'multiple $geoIntersects Polygon clauses not supported by $or. index type: ' + indexname);

t.dropIndexes();

var indexname = "2d";

t.ensureIndex({loc: indexname});

assert.eq(2,
          t.find({
               $or: [
                   {loc: {$geoWithin: {$centerSphere: [p, 10]}}},
                   {loc: {$geoWithin: {$centerSphere: [p, 10]}}}
               ]
           }).itcount(),
          'multiple $geoWithin clauses not supported by $or. index type: ' + indexname);
