// 32bit.js dm

// Use Random.rand() and helpers, not Math.random()

var abortSlowHost = true;
var forceSeedToBe = null;

if (forceSeedToBe) {
    print("\n32bit.js WARNING FORCING A SPECIFIC SEED");
    print("seed=" + forceSeedToBe);
    Random.srand(forceSeedToBe);
} else {
    Random.setRandomSeed();
}

function f() {
    'use strict';

    var pass = 1;
    var mydb = db.getSisterDB("test_32bit");
    var t = mydb.colltest_32bit;

    mydb.dropDatabase();

    while (1) {
        if (pass >= 2) {
            break;
        }
        print("32bit.js PASS #" + pass);
        pass++;

        t.insert({x: 1});
        t.ensureIndex({a: 1});
        t.ensureIndex({b: 1}, true);
        t.ensureIndex({x: 1});
        if (Random.rand() < 0.3) {
            t.ensureIndex({c: 1});
        }
        t.ensureIndex({d: 1});
        t.ensureIndex({e: 1});
        t.ensureIndex({f: 1});

        // create 448 byte string
        var big = 'a                          b';
        big = big + big;
        big = big + big;
        big = big + big;
        big = big + big;

        var a = 0;
        var c = 'kkk';
        var start = new Date();
        var b, d, f, cc;

        while (1) {
            // Insert:
            //   a: number, integer count of documents inserted
            //   b: number, random in range [0.0, 1.0)
            //   c: null (10% chance) or string big (90% chance)
            //   d: string "kkk-<value of a>"
            //   f: number, a + random in range [0.0, 1.0)

            b = Random.rand();
            d = c + -a;
            f = Random.rand() + a;
            a++;
            cc = big;
            if (Random.rand() < 0.1) {
                cc = null;
            }

            var res = t.insert({a: a, b: b, c: cc, d: d, f: f});
            if (res.hasWriteError()) {
                // Presumably we have mmap error on 32 bit. try a few more manipulations
                // attempting to break things.
                t.insert({a: 33, b: 44, c: 55, d: 66, f: 66});
                t.insert({a: 33, b: 44000, c: 55, d: 66});
                t.insert({a: 33, b: 440000, c: 55});
                t.insert({a: 33, b: 4400000});
                t.update({a: 20}, {'$set': {c: 'abc'}});
                t.update({a: 21}, {'$set': {c: 'aadsfbc'}});
                t.update({a: 22}, {'$set': {c: 'c'}});
                t.update({a: 23}, {'$set': {b: cc}});
                t.remove({a: 22});
                break;
            }

            if (Random.rand() < 0.01) {
                t.remove({a: a});
                t.remove({b: Random.rand()});
                t.insert({e: 1});
                t.insert({f: 'aaaaaaaaaa'});

                if (Random.rand() < 0.00001) {
                    print("remove cc");
                    t.remove({c: cc});
                }
                if (Random.rand() < 0.0001) {
                    print("update cc");
                    t.update({c: cc}, {'$set': {c: 1}}, false, true);
                }
                if (Random.rand() < 0.00001) {
                    print("remove e");
                    t.remove({e: 1});
                }
            }
            if (a == 20000) {
                var delta_ms = (new Date()) - start;
                // 2MM / 20000 = 100.  1000ms/sec.
                var eta_secs = delta_ms * (100 / 1000);
                print("32bit.js eta_secs:" + eta_secs);
                if (eta_secs > 1000 && abortSlowHost) {
                    print("32bit.js machine is slow, stopping early. a:" + a);
                    mydb.dropDatabase();
                    return;
                }
            }
            if (a % 100000 == 0) {
                print(a);
                // on 64 bit we won't error out, so artificially stop.  on 32 bit we will hit
                // mmap limit ~1.6MM but may vary by a factor of 2x by platform
                if (a >= 2200000) {
                    mydb.dropDatabase();
                    return;
                }
            }
        }
        print("count: " + t.count());

        var res = t.validate();
        if (!res.valid) {
            print("32bit.js FAIL validating");
            print(res.result);
            printjson(res);
            // mydb.dropDatabase();
            throw Error("fail validating 32bit.js");
        }

        mydb.dropDatabase();
    }

    print("32bit.js SUCCESS");
}

print("\n32bit.js running - this test is slow.");
f();
