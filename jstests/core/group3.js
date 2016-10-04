t = db.group3;
t.drop();

t.save({a: 1});
t.save({a: 2});
t.save({a: 3});
t.save({a: 4});

cmd = {
    initial: {count: 0, sum: 0},
    reduce: function(obj, prev) {
        prev.count++;
        prev.sum += obj.a;
    },
    finalize: function(obj) {
        if (obj.count) {
            obj.avg = obj.sum / obj.count;
        } else {
            obj.avg = 0;
        }
    },
};

result1 = t.group(cmd);

assert.eq(1, result1.length, "test1");
assert.eq(10, result1[0].sum, "test1");
assert.eq(4, result1[0].count, "test1");
assert.eq(2.5, result1[0].avg, "test1");

cmd['finalize'] = function(obj) {
    if (obj.count) {
        return obj.sum / obj.count;
    } else {
        return 0;
    }
};

result2 = t.group(cmd);

assert.eq(1, result2.length, "test2");
assert.eq(2.5, result2[0], "test2");
