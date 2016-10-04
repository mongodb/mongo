/*
 * SERVER-6147 : aggregation $ne expression applied to constant returns incorrect result
 *
 * This test validates the SERVER-6147 ticket. Return true when comparing a constant to a field
 * containing a different value using $ne. Previously it would return false when comparing a
 * constant and a field regardless of whether they were equal or not.
 */

/*
 * 1) Clear and create testing db
 * 2) Run an aggregation with $ne comparing constants and fields in various configurations
 * 3) Assert that the result is what we expected
 */

// Clear db
db.s6147.drop();

// Populate db
db.s6147.save({a: 1});
db.s6147.save({a: 2});

// Aggregate checking various combinations of the constant and the field
var s6147 = db.s6147.aggregate({
    $project: {
        _id: 0,
        constantAndField: {$ne: [1, "$a"]},
        fieldAndConstant: {$ne: ["$a", 1]},
        constantAndConstant: {$ne: [1, 1]},
        fieldAndField: {$ne: ["$a", "$a"]}
    }
});

/*
 * In both documents the constantAndConstant and fieldAndField should be false since they compare
 * something with itself but the constantAndField and fieldAndConstant should be different as
 * document one contains 1 which should return false and document 2 contains something different so
 * should return true
 */
var s6147result = [
    {
      constantAndField: false,
      fieldAndConstant: false,
      constantAndConstant: false,
      fieldAndField: false
    },
    {
      constantAndField: true,
      fieldAndConstant: true,
      constantAndConstant: false,
      fieldAndField: false
    }
];

// Assert
assert.eq(s6147.toArray(), s6147result, 's6147 failed');
