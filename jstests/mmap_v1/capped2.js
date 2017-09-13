db.capped2.drop();
db._dbCommand({create: "capped2", capped: true, size: 1000, $nExtents: 11, autoIndexId: false});
tzz = db.capped2;

function debug(x) {
    //    print( x );
}

var val = new Array(2000);
var c = "";
for (i = 0; i < 2000; ++i, c += "---") {  // bigger and bigger objects through the array...
    val[i] = {a: c};
}

function checkIncreasing(i) {
    res = tzz.find().sort({$natural: -1});
    assert(res.hasNext(), "A");
    var j = i;
    while (res.hasNext()) {
        try {
            assert.eq(val[j--].a, res.next().a, "B");
        } catch (e) {
            debug("capped2 err " + j);
            throw e;
        }
    }
    res = tzz.find().sort({$natural: 1});
    assert(res.hasNext(), "C");
    while (res.hasNext())
        assert.eq(val[++j].a, res.next().a, "D");
    assert.eq(j, i, "E");
}

function checkDecreasing(i) {
    res = tzz.find().sort({$natural: -1});
    assert(res.hasNext(), "F");
    var j = i;
    while (res.hasNext()) {
        assert.eq(val[j++].a, res.next().a, "G");
    }
    res = tzz.find().sort({$natural: 1});
    assert(res.hasNext(), "H");
    while (res.hasNext())
        assert.eq(val[--j].a, res.next().a, "I");
    assert.eq(j, i, "J");
}

for (i = 0;; ++i) {
    debug("capped 2: " + i);
    tzz.insert(val[i]);
    var err = db.getLastError();
    if (err) {
        debug(err);
        debug(tzz.count());
        assert(i > 100, "K");
        break;
    }
    checkIncreasing(i);
}

// drop and recreate. Test used to rely on the last insert emptying the collection, which it no
// longer does now that we rollback on failed inserts.
tzz.drop();
db._dbCommand({create: "capped2", capped: true, size: 1000, $nExtents: 11, autoIndexId: false});

for (i = 600; i >= 0; --i) {
    debug("capped 2: " + i);
    tzz.insert(val[i]);
    checkDecreasing(i);
}
