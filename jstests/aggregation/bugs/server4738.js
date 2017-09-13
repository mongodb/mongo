// test to make sure we accept all numeric types for inclusion
c = db.blah;
c.drop();
c.save({key: 4, v: 3, x: 2});

var r = c.aggregate({
    "$project": {
        "_id": 0,
        "key": NumberLong(1),
        "v": 1, /* javascript:  really a double */
        "x": NumberInt(1)
    }
});

assert.eq(r.toArray(), [{key: 4, v: 3, x: 2}], "support204 failed");
