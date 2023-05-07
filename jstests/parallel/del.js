load('jstests/libs/parallelTester.js');

const N = 1000;
const HOST = db.getMongo().host;

const a = db.getSiblingDB("fooa");
const b = db.getSiblingDB("foob");
a.dropDatabase();
b.dropDatabase();

var kCursorKilledErrorCodes = [
    ErrorCodes.OperationFailed,
    ErrorCodes.QueryPlanKilled,
    ErrorCodes.CursorNotFound,
];

function del1(dbname, host, max, kCursorKilledErrorCodes) {
    try {
        var m = new Mongo(host);
        var db = m.getDB("foo" + dbname);
        var t = db.del;

        while (!db.del_parallel.count()) {
            var r = Math.random();
            var n = Math.floor(Math.random() * max);
            if (r < .9) {
                t.insert({x: n});
            } else if (r < .98) {
                t.remove({x: n});
            } else if (r < .99) {
                t.remove({x: {$lt: n}});
            } else {
                t.remove({x: {$gt: n}});
            }
            if (r > .9999)
                print(t.count());
        }

        return {ok: 1};
    } catch (e) {
        if (kCursorKilledErrorCodes.includes(e.code)) {
            // It is expected that the cursor may have been killed due to the database being
            // dropped concurrently.
            return {ok: 1};
        }

        throw e;
    }
}

function del2(dbname, host, max, kCursorKilledErrorCodes) {
    try {
        var m = new Mongo(host);
        var db = m.getDB("foo" + dbname);
        var t = db.del;

        while (!db.del_parallel.count()) {
            var r = Math.random();
            var n = Math.floor(Math.random() * max);
            var s = Math.random() > .5 ? 1 : -1;

            if (r < .5) {
                t.findOne({x: n});
            } else if (r < .75) {
                t.find({x: {$lt: n}}).sort({x: s}).itcount();
            } else {
                t.find({x: {$gt: n}}).sort({x: s}).itcount();
            }
        }

        return {ok: 1};
    } catch (e) {
        if (kCursorKilledErrorCodes.includes(e.code)) {
            // It is expected that the cursor may have been killed due to the database being
            // dropped concurrently.
            return {ok: 1};
        }

        throw e;
    }
}

const all = [];

all.push(fork(del1, "a", HOST, N, kCursorKilledErrorCodes));
all.push(fork(del2, "a", HOST, N, kCursorKilledErrorCodes));
all.push(fork(del1, "b", HOST, N, kCursorKilledErrorCodes));
all.push(fork(del2, "b", HOST, N, kCursorKilledErrorCodes));

for (let i = 0; i < all.length; i++)
    all[i].start();

for (let i = 0; i < 10; i++) {
    sleep(2000);
    print("dropping");
    a.dropDatabase();
    b.dropDatabase();
}

a.del_parallel.save({done: 1});
b.del_parallel.save({done: 1});

for (let i = 0; i < all.length; i++) {
    assert.commandWorked(all[i].returnData());
}
