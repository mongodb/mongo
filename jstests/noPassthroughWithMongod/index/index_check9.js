// Randomized index testing

Random.setRandomSeed();

let t = db.test_index_check9;

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
    let alphas = [];
    for (var i = 0; i < n; ++i) {
        alphas.push(Random.rand() > 0.5);
    }

    t.createIndex(idx);

    function obj() {
        let ret = {};
        for (let i = 0; i < n; ++i) {
            ret[fields[i]] = r(alphas[i]);
        }
        return ret;
    }

    function r(alpha) {
        if (!alpha) {
            return Random.randInt(10);
        } else {
            let len = Random.randInt(10);
            let buf = "";
            for (let i = 0; i < len; ++i) {
                buf += chars.charAt(Random.randInt(chars.length));
            }
            return buf;
        }
    }

    function check() {
        let v = t.validate();
        if (!t.valid) {
            printjson(t);
            assert(t.valid);
        }
        let spec = {};
        for (let i = 0; i < n; ++i) {
            let predicateType = Random.randInt(4);
            switch (predicateType) {
                case 0 /* range */: {
                    let bounds = [r(alphas[i]), r(alphas[i])];
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
                    break;
                }
                case 1 /* $in */: {
                    let vals = [];
                    let inLength = Random.randInt(15);
                    for (let j = 0; j < inLength; ++j) {
                        vals.push(r(alphas[i]));
                    }
                    spec[fields[i]] = {$in: vals};
                    break;
                }
                case 2 /* equality */: {
                    spec[fields[i]] = r(alphas[i]);
                    break;
                }
                default:
                    /* no predicate */ break;
            }
        }
        s = sort();
        let c1 = t.find(spec, {_id: null}).sort(s).hint(idx).toArray();
        let c2 = t.find(spec, {_id: null}).sort(s).hint({$natural: 1}).toArray();
        let count = t.count(spec);
        assert.eq(c1, c2);
        assert.eq(c2.length, count);
    }

    let bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < 10000; ++i) {
        bulk.insert(obj());
        if (Random.rand() > 0.999) {
            print(i);
            assert.commandWorked(bulk.execute());
            check();
            bulk = t.initializeUnorderedBulkOp();
        }
    }

    bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < 100000; ++i) {
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
