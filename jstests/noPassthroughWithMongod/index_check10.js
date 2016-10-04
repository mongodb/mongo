// Randomized index testing with initial btree constructed using btree builder.
// Also uses large strings.

Random.setRandomSeed();

t = db.test_index_check10;

function doIt() {
    t.drop();

    function sort() {
        var sort = {};
        for (var i = 0; i < n; ++i) {
            sort[fields[i]] = Random.rand() > 0.5 ? 1 : -1;
        }
        return sort;
    }

    var fields = ['a', 'b', 'c', 'd', 'e'];
    n = Random.randInt(5) + 1;
    var idx = sort();

    var chars = "abcdefghijklmnopqrstuvwxyz";

    function obj() {
        var ret = {};
        for (var i = 0; i < n; ++i) {
            ret[fields[i]] = r();
        }
        return ret;
    }

    function r() {
        var len = Random.randInt(700 / n);
        buf = "";
        for (var i = 0; i < len; ++i) {
            buf += chars.charAt(Random.randInt(chars.length));
        }
        return buf;
    }

    function check() {
        var v = t.validate();
        if (!v.valid) {
            printjson(v);
            assert(v.valid);
        }
        var spec = {};
        for (var i = 0; i < n; ++i) {
            if (Random.rand() > 0.5) {
                var bounds = [r(), r()];
                if (bounds[0] > bounds[1]) {
                    bounds.reverse();
                }
                var s = {};
                if (Random.rand() > 0.5) {
                    s["$gte"] = bounds[0];
                } else {
                    s["$gt"] = bounds[0];
                }
                if (Random.rand() > 0.5) {
                    s["$lte"] = bounds[1];
                } else {
                    s["$lt"] = bounds[1];
                }
                spec[fields[i]] = s;
            } else {
                var vals = [];
                for (var j = 0; j < Random.randInt(15); ++j) {
                    vals.push(r());
                }
                spec[fields[i]] = {$in: vals};
            }
        }
        s = sort();
        c1 = t.find(spec, {_id: null}).sort(s).hint(idx).toArray();
        try {
            c3 = t.find(spec, {_id: null}).sort(s).hint({$natural: 1}).toArray();
        } catch (e) {
            // may assert if too much data for in memory sort
            print("retrying check...");
            check();  // retry with different bounds
            return;
        }

        var j = 0;
        for (var i = 0; i < c3.length; ++i) {
            if (friendlyEqual(c1[j], c3[i])) {
                ++j;
            } else {
                var o = c3[i];
                var size = Object.bsonsize(o);
                for (var f in o) {
                    size -= f.length;
                }

                var max = 818;  // KeyMax
                if (size <= max) {
                    assert.eq(c1, c3, "size: " + size);
                }
            }
        }
    }

    var bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < 10000; ++i) {
        bulk.insert(obj());
    }
    assert.writeOK(bulk.execute());

    t.ensureIndex(idx);
    check();

    bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < 10000; ++i) {
        if (Random.rand() > 0.9) {
            bulk.insert(obj());
        } else {
            bulk.find(obj()).remove();  // improve
        }
        if (Random.rand() > 0.999) {
            print(i);
            assert.writeOK(bulk.execute());
            check();
            bulk = t.initializeUnorderedBulkOp();
        }
    }
    assert.writeOK(bulk.execute());
    check();
}

for (var z = 0; z < 5; ++z) {
    doIt();
}
