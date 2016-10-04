// SERVER-2585 Test $or clauses within indexed top level $or clauses.

t = db.jstests_ork;
t.drop();

t.ensureIndex({a: 1});
t.save({a: [1, 2], b: 5});
t.save({a: [2, 4], b: 5});

assert.eq(2, t.find({
                  $or: [
                      {a: 1, $and: [{$or: [{a: 2}, {a: 3}]}, {$or: [{b: 5}]}]},
                      {a: 2, $or: [{a: 3}, {a: 4}]}
                  ]
              }).itcount());
assert.eq(1, t.find({
                  $or: [
                      {a: 1, $and: [{$or: [{a: 2}, {a: 3}]}, {$or: [{b: 6}]}]},
                      {a: 2, $or: [{a: 3}, {a: 4}]}
                  ]
              }).itcount());
