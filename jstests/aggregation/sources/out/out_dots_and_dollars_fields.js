// Tests that $out is allowed to write documents containing fields with dots and dollars in them.
//
// This test assumes that collections are not implicitly sharded, since $out is prohibited if the
// output collection is sharded.
// @tags: [assumes_unsharded_collection]
(function() {
"use strict";

const coll = db.out_dots_and_dollars_fields_coll;
coll.drop();
coll.insertOne({_id: 0});

const from = db.out_dots_and_dollars_fields_from;
from.drop();
from.insertOne({_id: 0});

// Test that the 'on' field can contain dots and dollars when the appropriate feature flag is on.
const isDotsAndDollarsEnabled = db.adminCommand({getParameter: 1, featureFlagDotsAndDollars: 1})
                                    .featureFlagDotsAndDollars.value;
if (isDotsAndDollarsEnabled) {
    const test = {
        _id: 0,
        "$aField": 1,
        "aFie$ld": 1,
        "aField$": 1,
        "$$aField": 1,
        ".aField": 1,
        "aFie.ld": 1,
        "aField.": 1,
        "..aField": 1,
        "$.aField": 1,
        "aFi.$eld": 1,
    };
    [coll, from].forEach((c) => {
        assert.doesNotThrow(
            () => coll.aggregate([{$replaceWith: {$const: test}}, {$out: c.getName()}]));
        assert.eq(c.find().toArray(), [test]);
    });
}
}());
