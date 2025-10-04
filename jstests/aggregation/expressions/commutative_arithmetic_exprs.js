// SERVER-42756 Test that commutative arithmetic operations with special arguments doesn't violate
// commutativity.
const coll = db[jsTest.name()];
coll.drop();
const numbers = [1.0, NumberInt("1"), NumberLong("1"), NumberDecimal("1.0")];
const specials = [
    {val: NaN, path: "$nan"},
    {val: Infinity, path: "$inf"},
];

assert.commandWorked(coll.insert({inf: Infinity, nan: NaN}));

["$multiply", "$add", "$sum"].forEach((op) => {
    (function testCommutativityWithConstArguments() {
        specials.forEach((special) => {
            numbers.forEach((num) => {
                const expected = [{a: num instanceof NumberDecimal ? NumberDecimal(special.val) : special.val}];
                assert.eq(expected, coll.aggregate([{$project: {a: {[op]: [special.val, num]}, _id: 0}}]).toArray());
                assert.eq(expected, coll.aggregate([{$project: {a: {[op]: [num, special.val]}, _id: 0}}]).toArray());
            });
        });
    })();

    (function testCommutativityWithNonConstArgument() {
        specials.forEach((special) => {
            numbers.forEach((num) => {
                const expected = [{a: num instanceof NumberDecimal ? NumberDecimal(special.val) : special.val}];
                assert.eq(expected, coll.aggregate([{$project: {a: {[op]: [special.path, num]}, _id: 0}}]).toArray());
                assert.eq(expected, coll.aggregate([{$project: {a: {[op]: [num, special.path]}, _id: 0}}]).toArray());
            });
        });
    })();
});
