// Reported as server-848.
// @tags: [
//   requires_getmore,
// ]
function test(index) {
    db.server848.drop();

    let radius = 0.0001;
    let center = [5, 52];

    db.server848.save({"_id": 1, "loc": {"x": 4.9999, "y": 52}});
    db.server848.save({"_id": 2, "loc": {"x": 5, "y": 52}});
    db.server848.save({"_id": 3, "loc": {"x": 5.0001, "y": 52}});
    db.server848.save({"_id": 4, "loc": {"x": 5, "y": 52.0001}});
    db.server848.save({"_id": 5, "loc": {"x": 5, "y": 51.9999}});
    db.server848.save({"_id": 6, "loc": {"x": 4.9999, "y": 52.0001}});
    db.server848.save({"_id": 7, "loc": {"x": 5.0001, "y": 52.0001}});
    db.server848.save({"_id": 8, "loc": {"x": 4.9999, "y": 51.9999}});
    db.server848.save({"_id": 9, "loc": {"x": 5.0001, "y": 51.9999}});
    if (index) {
        db.server848.createIndex({loc: "2d"});
    }
    let r = db.server848.find({"loc": {"$within": {"$center": [center, radius]}}}, {_id: 1});
    assert.eq(5, r.count(), "A1");
    // FIXME: surely code like this belongs in utils.js.
    let a = r.toArray();
    let x = [];
    for (let k in a) {
        x.push(a[k]["_id"]);
    }
    x.sort();
    assert.eq([1, 2, 3, 4, 5], x, "B1");
}

test(false);
test(true);
