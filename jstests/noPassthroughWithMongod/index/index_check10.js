// Randomized index testing with initial btree constructed using btree builder.
// Also uses large strings.

Random.setRandomSeed();

let t = db.test_index_check10;

function doIt() {
    t.drop();

    function sort() {
        let sort = {};
        for (let i = 0; i < n; ++i) {
            sort[fields[i]] = Random.rand() > 0.5 ? 1 : -1;
        }
        return sort;
    }

    var fields = ["a", "b", "c", "d", "e"];
    let n = Random.randInt(5) + 1;
    let idx = sort();

    let chars = "abcdefghijklmnopqrstuvwxyz";

    function obj() {
        let ret = {};
        for (let i = 0; i < n; ++i) {
            ret[fields[i]] = r();
        }
        return ret;
    }

    function r() {
        let len = Random.randInt(700 / n);
        let buf = "";
        for (let i = 0; i < len; ++i) {
            buf += chars.charAt(Random.randInt(chars.length));
        }
        return buf;
    }

    function check() {
        let v = t.validate();
        if (!v.valid) {
            printjson(v);
            assert(v.valid);
        }
        let spec = {};
        for (var i = 0; i < n; ++i) {
            if (Random.rand() > 0.5) {
                let bounds = [r(), r()];
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
                let vals = [];
                for (var j = 0; j < Random.randInt(15); ++j) {
                    vals.push(r());
                }
                spec[fields[i]] = {$in: vals};
            }
        }
        s = sort();
        let c1 = t.find(spec, {_id: null}).sort(s).hint(idx).toArray();
        try {
            var c3 = t.find(spec, {_id: null}).sort(s).hint({$natural: 1}).toArray();
        } catch (e) {
            // may assert if too much data for in memory sort
            print("retrying check...");
            check(); // retry with different bounds
            return;
        }

        var j = 0;
        for (var i = 0; i < c3.length; ++i) {
            if (friendlyEqual(c1[j], c3[i])) {
                ++j;
            } else {
                let o = c3[i];
                let size = Object.bsonsize(o);
                for (let f in o) {
                    size -= f.length;
                }

                let max = 818; // KeyMax
                if (size <= max) {
                    assert.eq(c1, c3, "size: " + size);
                }
            }
        }
    }

    let bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < 10000; ++i) {
        bulk.insert(obj());
    }
    assert.commandWorked(bulk.execute());

    t.createIndex(idx);
    check();

    bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < 10000; ++i) {
        if (Random.rand() > 0.9) {
            bulk.insert(obj());
        } else {
            bulk.find(obj()).remove(); // improve
        }
        if (Random.rand() > 0.999) {
            print(i);
            assert.commandWorked(bulk.execute());
            check();
            bulk = t.initializeUnorderedBulkOp();
        }
    }
    assert.commandWorked(bulk.execute());
    check();
}

for (let z = 0; z < 5; ++z) {
    doIt();
}
