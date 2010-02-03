t = db.group2;
t.drop();

t.save({a: 2});
t.save({b: 5});
t.save({a: 1});

cmd = { key: {a: 1},
        initial: {count: 0},
        reduce: function(obj, prev) {
            prev.count++;
        }
      };

result = t.group(cmd);

assert.eq(3, result.length, "A");
assert.eq(null, result[1].a, "C");
assert("a" in result[1], "D");
assert.eq(1, result[2].a, "E");

assert.eq(1, result[0].count, "F");
assert.eq(1, result[1].count, "G");
assert.eq(1, result[2].count, "H");


delete cmd.key
cmd["$keyf"] = function(x){ return { a : x.a }; };
result2 = t.group( cmd );

assert.eq( result , result2, "check result2" );


delete cmd.$keyf
cmd["keyf"] = function(x){ return { a : x.a }; };
result3 = t.group( cmd );

assert.eq( result , result3, "check result3" );
