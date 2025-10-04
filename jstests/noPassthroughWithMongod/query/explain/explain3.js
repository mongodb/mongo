// SERVER-2810 - similar to explain1 test, but with a scan and order find

let t = db.jstests_slowNightly_explain3;
t.drop();

// Periodically drops the collection, invalidating client cursors for s2's operations.
let s1 = startParallelShell(function () {
    t = db.jstests_slowNightly_explain3;
    for (let i = 0; i < 80; ++i) {
        t.drop();
        t.createIndex({x: 1});
        for (let j = 0; j < 1000; ++j) {
            t.save({x: j, y: 1});
        }
        sleep(100);
    }
});

// Query repeatedly.
let s2 = startParallelShell(function () {
    t = db.jstests_slowNightly_explain3;
    for (let i = 0; i < 500; ++i) {
        try {
            let z = t
                .find({x: {$gt: 0}, y: 1})
                .sort({x: 1})
                .explain();
        } catch (e) {}
    }
});

// Put pressure on s2 to yield more often.
let s3 = startParallelShell(function () {
    t = db.jstests_slowNightly_explain3;
    for (let i = 0; i < 200; ++i) {
        t.validate({scandata: true});
    }
});

s1();
s2();
s3();
