// Test correctness of the "keys" and and "count" fields in the group command output.
var coll = db.group8;
var result;

coll.drop();
assert.writeOK(coll.insert({a: 1, b: "x"}));
assert.writeOK(coll.insert({a: 2, b: "x"}));
assert.writeOK(coll.insert({a: 2, b: "x"}));
assert.writeOK(coll.insert({a: 3, b: "y"}));

// Test case when "count" and "keys" are both zero.
result = coll.runCommand({
    group: {
        ns: coll.getName(),
        key: {a: 1},
        cond: {b: "z"},
        $reduce: function(x, y) {},
        initial: {}
    }
});
assert.commandWorked(result);
assert.eq(result.count, 0);
assert.eq(result.keys, 0);
assert.eq(result.retval.length, 0);

// Test case when "count" and "keys" are both non-zero.
result = coll.runCommand({
    group: {
        ns: coll.getName(),
        key: {a: 1},
        cond: {b: "x"},
        $reduce: function(x, y) {},
        initial: {}
    }
});
assert.commandWorked(result);
assert.eq(result.count, 3);
assert.eq(result.keys, 2);
assert.eq(result.retval.length, 2);
