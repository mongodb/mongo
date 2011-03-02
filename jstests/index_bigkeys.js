
t = db.bigkeysidxtest;

var keys = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];

var str = "aaaabbbbccccddddeeeeffffgggghhhh";
str = str + str;

for (var i = 2; i < 10; i++) {
    keys[i] = str;
    str = str + str;
}
print(str.length);

var dir = 1;

function go() {
    if (dir == 1) {
        for (var i = 1; i < 10; i++) {
            t.insert({ _id: i, k: keys[i] });
        }
    }
    else {
        for (var i = 10; i >= 1; i--) {
            t.insert({ _id: i, k: keys[i] });
        }
    }
}

var expect = null;

var ok = true;

function check() {
    assert(t.validate().valid);

    var c = t.find({ k: /^a/ }).count();

    print("keycount:" + c);

    if (expect) {
        if (expect != c) {
            print("count of keys doesn't match expected count of : " + expect + " got: " + c);
            ok = false;
        }
    }
    else {
        expect = c;
    }

    //print(t.validate().result);
}

for (var pass = 1; pass <= 2; pass++) {
    print("pass:" + pass);

    t.drop();
    t.ensureIndex({ k: 1 });
    go();
    check(); // check incremental addition

    t.reIndex();
    check(); // check bottom up

    t.drop();
    go();
    t.ensureIndex({ k: 1 });
    check(); // check bottom up again without reindex explicitly

    t.drop();
    go();
    t.ensureIndex({ k: 1 }, { background: true });
    check(); // check background (which should be incremental)

    dir = -1;
}

assert(ok,"not ok");
