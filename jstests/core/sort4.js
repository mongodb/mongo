(function() {
    "use strict";

    const coll = db.sort4;
    coll.drop();

    function nice(sort, correct, extra) {
        const c = coll.find().sort(sort);
        let s = "";
        c.forEach(function(z) {
            if (s.length) {
                s += ",";
            }
            s += z.name;
            if (z.prename) {
                s += z.prename;
            }
        });
        if (correct) {
            assert.eq(correct, s, tojson(sort) + "(" + extra + ")");
        }
        return s;
    }

    assert.writeOK(coll.insert({name: 'A', prename: 'B'}));
    assert.writeOK(coll.insert({name: 'A', prename: 'C'}));
    assert.writeOK(coll.insert({name: 'B', prename: 'B'}));
    assert.writeOK(coll.insert({name: 'B', prename: 'D'}));

    nice({name: 1, prename: 1}, "AB,AC,BB,BD", "s3");
    nice({prename: 1, name: 1}, "AB,BB,AC,BD", "s3");

    assert.writeOK(coll.insert({name: 'A'}));
    nice({name: 1, prename: 1}, "A,AB,AC,BB,BD", "e1");

    assert.writeOK(coll.insert({name: 'C'}));
    nice({name: 1, prename: 1}, "A,AB,AC,BB,BD,C", "e2");  // SERVER-282

    assert.commandWorked(coll.ensureIndex({name: 1, prename: 1}));
    nice({name: 1, prename: 1}, "A,AB,AC,BB,BD,C", "e2ia");  // SERVER-282

    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.ensureIndex({name: 1}));
    nice({name: 1, prename: 1}, "A,AB,AC,BB,BD,C", "e2ib");  // SERVER-282
}());
