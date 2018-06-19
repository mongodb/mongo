(function() {
    "use strict";
    var t = db.geob;
    t.drop();

    var a = {p: [0, 0]};
    var b = {p: [1, 0]};
    var c = {p: [3, 4]};
    var d = {p: [0, 6]};

    t.save(a);
    t.save(b);
    t.save(c);
    t.save(d);
    t.ensureIndex({p: "2d"});

    let res = t.aggregate({$geoNear: {near: [0, 0], distanceField: "dis"}}).toArray();

    assert.close(0, res[0].dis, "B1");
    assert.eq(a._id, res[0]._id, "B2");

    assert.close(1, res[1].dis, "C1");
    assert.eq(b._id, res[1]._id, "C2");

    assert.close(5, res[2].dis, "D1");
    assert.eq(c._id, res[2]._id, "D2");

    assert.close(6, res[3].dis, "E1");
    assert.eq(d._id, res[3]._id, "E2");

    res = t.aggregate({
               $geoNear: {near: [0, 0], distanceField: "dis", distanceMultiplier: 2.0}
           }).toArray();
    assert.close(0, res[0].dis, "G");
    assert.close(2, res[1].dis, "H");
    assert.close(10, res[2].dis, "I");
    assert.close(12, res[3].dis, "J");
}());
