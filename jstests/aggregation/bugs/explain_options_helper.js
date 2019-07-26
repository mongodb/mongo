// Test that the explain helper does not modify the options document passed to it.
// This test was designed to reproduce SERVER-32300".

(function() {
"use strict";

const coll = db.explain_options;
coll.drop();

for (let i = 0; i < 10; ++i) {
    assert.writeOK(coll.insert({_id: i}));
}

const collation = {
    collation: {locale: "zh", backwards: false}
};

const firstResults = coll.aggregate([{$sort: {_id: 1}}], collation).toArray();
// Issue an explain in order to verify that 'collation' is not modified to include the explain
// flag.
assert.commandWorked(coll.explain().aggregate([], collation));

const secondResults = coll.aggregate([{$sort: {_id: 1}}], collation).toArray();
// Assert that the result didn't change after an explain helper is issued.
assert.eq(firstResults, secondResults);
}());
