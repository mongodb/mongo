// @tags: [
//   requires_non_retryable_writes
//]

let t = db.jstests_js2;
t.remove({});

let t2 = db.jstests_js2_2;
t2.remove({});

assert.eq(0, t2.find().length(), "A");

t.save({z: 1});
t.save({z: 2});
assert.throws(
    function () {
        t.find({
            $where: function () {
                db.jstests_js2_2.save({y: 1});
                return 1;
            },
        }).forEach(printjson);
    },
    [],
    "can't save from $where",
);

assert.eq(0, t2.find().length(), "B");

assert(t.validate().valid, "E");
