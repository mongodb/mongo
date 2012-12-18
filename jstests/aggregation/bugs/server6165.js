/*
 * SERVER-6165: use of an integer valued double in a $mod expression can cause an incorrect result
 *
 * This test validates the SERVER-6165 ticket. Return a long if $mod is operating on a long and a
 * double. Previously it would be forced into an int and convert improperly.
 */

/*
 * 1) Clear and create testing db
 * 2) Run aggregations testing various combinations of number types with the $mod operator
 * 3) Assert that the results are what we expected
 */

// Clear db
db.s6165.drop();
db.s6165.save({});

// Aggregate checking various combinations of number types
// The $match portion ensures they are of the correct type as the shell turns 
// the ints back to doubles at the end so we can not check types with asserts
var s6165 = db.s6165.aggregate(
        { $project: {
            _id: 0,
            dub_dub: {$mod: [138.5, 3.0]},
            dub_int: {$mod: [138.5, NumberLong(3)]},
            dub_long: {$mod: [138.5, NumberInt(3)]},
            int_dub: {$mod: [NumberInt(8), 3.25]},
            int_dubint: {$mod: [NumberInt(8), 3.0]},
            int_int: {$mod: [NumberInt(8), NumberInt(3)]},
            int_long: {$mod: [NumberInt(8), NumberLong(3)]},
            long_dub: {$mod: [NumberLong(8), 3.25]},
            long_dubint: {$mod: [NumberLong(8), 3.0]},
            long_dublong: {$mod: [NumberLong(500000000000), 450000000000.0]},
            long_int: {$mod: [NumberLong(8), NumberInt(3)]},
            long_long: {$mod: [NumberLong(8), NumberLong(3)]},
            verylong_verylong: {$mod: [NumberLong(800000000000), NumberLong(300000000000)]}
        }},
        { $match: {
            // 1 is NumberDouble
            dub_dub: {$type: 1},
            dub_int: {$type: 1},
            dub_long: {$type: 1},
            int_dub: {$type: 1},
            // 16 is NumberInt
            int_dubint: {$type: 16},
            int_int: {$type: 16},
            // 18 is NumberLong
            int_long: {$type: 18},
            long_dub: {$type: 1},
            long_dubint: {$type: 18},
            long_dublong: {$type: 1},
            long_int: {$type: 18},
            long_long: {$type: 18},
            verylong_verylong: {$type: 18}
        }}
);

// Correct answers (it is mainly the types that are important here)
var s6165result = [
    {
        dub_dub: 0.5,
        dub_int: 0.5,
        dub_long: 0.5,
        int_dub: 1.5,
        int_dubint: 2,
        int_int: 2,
        int_long: NumberLong(2),
        long_dub: 1.5,
        long_dubint: NumberLong(2),
        long_dublong: 50000000000,
        long_int: NumberLong(2),
        long_long: NumberLong(2),
        verylong_verylong: NumberLong(200000000000)
    }
];

// Assert
assert.eq(s6165.result, s6165result, 's6165 failed');
