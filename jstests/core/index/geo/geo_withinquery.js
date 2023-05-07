// @tags: [
//   requires_getmore,
// ]

// SERVER-7343: allow $within without a geo index.
let t = db.geo_withinquery;
t.drop();

let num = 0;
for (let x = 0; x <= 20; x++) {
    for (let y = 0; y <= 20; y++) {
        let o = {_id: num++, loc: [x, y]};
        t.save(o);
    }
}

assert.eq(21 * 21 - 1,
          t.find({
               $and: [
                   {loc: {$ne: [0, 0]}},
                   {loc: {$within: {$box: [[0, 0], [100, 100]]}}},
               ]
           }).itcount(),
          "UHOH!");
