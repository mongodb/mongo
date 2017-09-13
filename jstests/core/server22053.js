(function() {

    "use strict";
    var t = db.jstests_server22053;

    /* eslint-disable no-sparse-arrays */
    var s0 = [, , 3, , , 6];
    t.coll.insert({mys: s0});

    var cur = t.coll.find();
    var doc = cur.next();
    assert.eq(6, doc['mys'].length);
    assert.eq(undefined, doc['mys'][0]);
    assert.eq(undefined, doc['mys'][1]);
    assert.eq(3, doc['mys'][2]);
    assert.eq(undefined, doc['mys'][3]);
    assert.eq(undefined, doc['mys'][4]);
    assert.eq(6, doc['mys'][5]);
}());