// @tags: [
//   does_not_support_stepdowns,
//   requires_getmore,
//   requires_non_retryable_writes,
//   requires_fastcount,
// ]

(function() {
    "use strict";

    const coll = db.or4;
    coll.drop();

    coll.ensureIndex({a: 1});
    coll.ensureIndex({b: 1});

    assert.writeOK(coll.insert({a: 2}));
    assert.writeOK(coll.insert({b: 3}));
    assert.writeOK(coll.insert({b: 3}));
    assert.writeOK(coll.insert({a: 2, b: 3}));

    assert.eq(4, coll.count({$or: [{a: 2}, {b: 3}]}));
    assert.eq(2, coll.count({$or: [{a: 2}, {a: 2}]}));

    assert.eq(2, coll.find({}).skip(2).count(true));
    assert.eq(2, coll.find({$or: [{a: 2}, {b: 3}]}).skip(2).count(true));
    assert.eq(1, coll.find({$or: [{a: 2}, {b: 3}]}).skip(3).count(true));

    assert.eq(2, coll.find({}).limit(2).count(true));
    assert.eq(1, coll.find({$or: [{a: 2}, {b: 3}]}).limit(1).count(true));
    assert.eq(2, coll.find({$or: [{a: 2}, {b: 3}]}).limit(2).count(true));
    assert.eq(3, coll.find({$or: [{a: 2}, {b: 3}]}).limit(3).count(true));
    assert.eq(4, coll.find({$or: [{a: 2}, {b: 3}]}).limit(4).count(true));

    coll.remove({$or: [{a: 2}, {b: 3}]});
    assert.eq(0, coll.count());

    assert.writeOK(coll.insert({b: 3}));
    coll.remove({$or: [{a: 2}, {b: 3}]});
    assert.eq(0, coll.count());

    assert.writeOK(coll.insert({a: 2}));
    assert.writeOK(coll.insert({b: 3}));
    assert.writeOK(coll.insert({a: 2, b: 3}));

    coll.update({$or: [{a: 2}, {b: 3}]}, {$set: {z: 1}}, false, true);
    assert.eq(3, coll.count({z: 1}));

    assert.eq(3, coll.find({$or: [{a: 2}, {b: 3}]}).toArray().length);
    assert.eq(coll.find().sort({_id: 1}).toArray(),
              coll.find({$or: [{a: 2}, {b: 3}]}).sort({_id: 1}).toArray());
    assert.eq(2, coll.find({$or: [{a: 2}, {b: 3}]}).skip(1).toArray().length);

    assert.eq(3, coll.find({$or: [{a: 2}, {b: 3}]}).batchSize(2).toArray().length);

    assert.writeOK(coll.insert({a: 1}));
    assert.writeOK(coll.insert({b: 4}));
    assert.writeOK(coll.insert({a: 2}));

    assert.eq(4, coll.find({$or: [{a: 2}, {b: 3}]}).batchSize(2).toArray().length);

    assert.writeOK(coll.insert({a: 1, b: 3}));
    assert.eq(4, coll.find({$or: [{a: 2}, {b: 3}]}).limit(4).toArray().length);

    assert.eq([1, 2], Array.sort(coll.distinct('a', {$or: [{a: 2}, {b: 3}]})));

    assert.eq(5,
              coll.mapReduce(
                      function() {
                          emit('a', this.a);
                      },
                      function(key, vals) {
                          return vals.length;
                      },
                      {out: {inline: true}, query: {$or: [{a: 2}, {b: 3}]}})
                  .counts.input);

    coll.remove({});

    assert.writeOK(coll.insert({a: [1, 2]}));
    assert.eq(1, coll.find({$or: [{a: 1}, {a: 2}]}).toArray().length);
    assert.eq(1, coll.count({$or: [{a: 1}, {a: 2}]}));
    assert.eq(1, coll.find({$or: [{a: 2}, {a: 1}]}).toArray().length);
    assert.eq(1, coll.count({$or: [{a: 2}, {a: 1}]}));
})();
