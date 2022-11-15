/**
 * Test that a window of the form [+N, unbounded] does not trigger a tassert
 * on mixed-type input.
 *
 * Originally intended to reproduce SERVER-71387.
 */
(function() {
"use strict";

const coll = db.set_window_fields_range_wrong_type;
coll.drop();
assert.commandWorked(coll.insert([
    // Numbers sort before strings, so we'll scan {a: 2} first.
    {a: 2},
    {a: 'xyz'},
]));

const err = assert.throws(() => {
    return coll
        .aggregate({
            $setWindowFields: {
                sortBy: {a: 1},
                output: {
                    // The lower bound +3 excludes the current document {a: 2}.
                    // The only remaining document is {a: 'xyz'}.
                    // We wrongly consider {a: 'xyz'} to be 'within' the lower bound.
                    // Then, when we search for the upper bound, we are surprised
                    // to be starting from 'xyz' which is the wrong type.
                    b: {$max: 5, window: {range: [+3, 'unbounded']}},
                },
            }
        })
        .toArray();
});
assert.eq(err.code, 5429414, err);
})();
