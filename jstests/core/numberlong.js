assert.eq.automsg("0", "new NumberLong()");

n = new NumberLong(4);
assert.eq.automsg("4", "n");
assert.eq.automsg("4", "n.toNumber()");
assert.eq.automsg("8", "n + 4");
assert.eq.automsg("'NumberLong(4)'", "n.toString()");
assert.eq.automsg("'NumberLong(4)'", "tojson( n )");
a = {};
a.a = n;
p = tojson(a);
assert.eq.automsg("'{ \"a\" : NumberLong(4) }'", "p");

assert.eq.automsg("NumberLong(4 )", "eval( tojson( NumberLong( 4 ) ) )");
assert.eq.automsg("a", "eval( tojson( a ) )");

n = new NumberLong(-4);
assert.eq.automsg("-4", "n");
assert.eq.automsg("-4", "n.toNumber()");
assert.eq.automsg("0", "n + 4");
assert.eq.automsg("'NumberLong(-4)'", "n.toString()");
assert.eq.automsg("'NumberLong(-4)'", "tojson( n )");
a = {};
a.a = n;
p = tojson(a);
assert.eq.automsg("'{ \"a\" : NumberLong(-4) }'", "p");

// double
n = new NumberLong(4294967296);  // 2^32
assert.eq.automsg("4294967296", "n");
assert.eq.automsg("4294967296", "n.toNumber()");
assert.eq.automsg("4294967295", "n - 1");
assert.eq.automsg("'NumberLong(\"4294967296\")'", "n.toString()");
assert.eq.automsg("'NumberLong(\"4294967296\")'", "tojson( n )");
assert.eq.automsg("4294967296", "n.floatApprox");
assert.eq.automsg("1", "n.top");
assert.eq.automsg("0", "n.bottom");
a = {};
a.a = n;
p = tojson(a);
assert.eq.automsg("'{ \"a\" : NumberLong(\"4294967296\") }'", "p");

var goodValues = [
    0,
    "9223372036854775807",   // int64_t max
    "-9223372036854775808",  // int64_t min
    -9223372036854776000,
    9223372036854775000,
];
var badNum = "number passed to NumberLong must be representable as an int64_t";
var badStr = "could not convert string to long long";
var badValues = [
    {val: NaN, msg: badNum},
    {val: "9223372036854775808", msg: badStr},  // int64_t max + 1
    {val: 9223372036854776000, msg: badNum},
    {val: "-9223372036854775809", msg: badStr},  // int64_t min - 1
    {val: -9223372036854778000, msg: badNum},
];

for (var i = 0; i < goodValues.length; i++) {
    try {
        NumberLong(goodValues[i]);
    } catch (e) {
        doassert("Error: NumberLong(" + goodValues[i] + ") should have worked, but got '" +
                 e.message + "'.");
    }
}
for (var i = 0; i < badValues.length; i++) {
    try {
        NumberLong(badValues[i].val);
        doassert("Error: NumberLong(" + badValues[i] + ") should have failed.");
    } catch (e) {
        assert.eq(e.message, badValues[i].msg);
    }
}

// too big to fit in double
n = new NumberLong("11111111111111111");
assert.eq.automsg("11111111111111112", "n.toNumber()");
assert.eq.automsg("11111111111111116", "n + 4");
assert.eq.automsg("'NumberLong(\"11111111111111111\")'", "n.toString()");
assert.eq.automsg("'NumberLong(\"11111111111111111\")'", "tojson( n )");
a = {};
a.a = n;
p = tojson(a);
assert.eq.automsg("'{ \"a\" : NumberLong(\"11111111111111111\") }'", "p");

assert.eq.automsg("NumberLong('11111111111111111' )",
                  "eval( tojson( NumberLong( '11111111111111111' ) ) )");
assert.eq.automsg("a", "eval( tojson( a ) )");

n = new NumberLong("-11111111111111111");
assert.eq.automsg("-11111111111111112", "n.toNumber()");
assert.eq.automsg("-11111111111111108", "n + 4");
assert.eq.automsg("'NumberLong(\"-11111111111111111\")'", "n.toString()");
assert.eq.automsg("'NumberLong(\"-11111111111111111\")'", "tojson( n )");
assert.eq.automsg("-11111111111111112", "n.floatApprox");
assert.eq.automsg("4292380288", "n.top");
assert.eq.automsg("3643379257", "n.bottom");
a = {};
a.a = n;
p = tojson(a);
assert.eq.automsg("'{ \"a\" : NumberLong(\"-11111111111111111\") }'", "p");

n = new NumberLong("9223372036854775807");
assert.eq.automsg("9223372036854775807", "n.floatApprox");
assert.eq.automsg("2147483647", "n.top");
assert.eq.automsg("4294967295", "n.bottom");

// From top and bottom
n = new NumberLong(9223372036854775807, 2147483647, 4294967295);
assert.eq.automsg("9223372036854775807", "n.floatApprox");
assert.eq.automsg("2147483647", "n.top");
assert.eq.automsg("4294967295", "n.bottom");

n = new NumberLong(0, 1, 0);  // Test that floatApprox argument is ignored.
assert.eq.automsg("4294967296", "n.floatApprox");
assert.eq.automsg("1", "n.top");
assert.eq.automsg("0", "n.bottom");

badValues = [
    [0, 4294967296, 0],
    [0, 0, 4294967296],
    ['asdf', 0, 0],
    [0, 1.5, 0],
];
for (var i = 0; i < badValues.length; i++) {
    assert.throws(function() {
        NumberLong.apply(null, badValues[i]);
    }, [], "Bad arguments to NumberLong should have thrown: " + JSON.stringify(badValues[i]));
}

// parsing
assert.throws.automsg(function() {
    new NumberLong("");
});
assert.throws.automsg(function() {
    new NumberLong("y");
});
assert.throws.automsg(function() {
    new NumberLong("11111111111111111111");
});

// create NumberLong from NumberInt (SERVER-9973)
assert.doesNotThrow.automsg(function() {
    new NumberLong(NumberInt(1));
});

// check that creating a NumberLong from a NumberLong bigger than a double doesn't
// get a truncated value (SERVER-9973)
n = new NumberLong(NumberLong("11111111111111111"));
assert.eq.automsg("n.toString()", "'NumberLong(\"11111111111111111\")'");

//
// Test NumberLong.compare()
//

var left = new NumberLong("0");
var right = new NumberLong("0");
assert.eq(left.compare(right), 0);
assert.eq(right.compare(left), 0);

left = new NumberLong("20");
right = new NumberLong("10");
assert.gt(left.compare(right), 0);
assert.lt(right.compare(left), 0);

left = new NumberLong("-9223372036854775808");
right = new NumberLong("9223372036854775807");
assert.lt(left.compare(right), 0);
assert.gt(right.compare(left), 0);
assert.eq(left.compare(left), 0);
assert.eq(right.compare(right), 0);

// Bad input to .compare().
assert.throws(function() {
    NumberLong("0").compare();
});
assert.throws(function() {
    NumberLong("0").compare(null);
});
assert.throws(function() {
    NumberLong("0").compare(undefined);
});
assert.throws(function() {
    NumberLong("0").compare(3);
});
assert.throws(function() {
    NumberLong("0").compare("foo");
});
assert.throws(function() {
    NumberLong("0").compare(NumberLong("0"), 3);
});
assert.throws(function() {
    NumberLong("0").compare({'replSet2Members': 6});
});

// Test auto complete
var getCompletions = function(prefix) {
    shellAutocomplete(prefix);
    return __autocomplete__;
};

var myNumberLong = new NumberLong();
var completions = getCompletions('myNumberLong.');
assert(completions.indexOf('myNumberLong.floatApprox') >= 0);
assert(completions.indexOf('myNumberLong.top') >= 0);
assert(completions.indexOf('myNumberLong.bottom') >= 0);
