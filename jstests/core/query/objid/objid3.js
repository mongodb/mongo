// @tags: [
//   # Time series collections may return fields in a different order due to internal rewriting or view semantics.
//   exclude_from_timeseries_crud_passthrough,
// ]

let t = db.objid3;
t.drop();

t.save({a: "bob", _id: 517});
for (let k in t.findOne()) {
    assert.eq(k, "_id", "keys out of order");
    break;
}
