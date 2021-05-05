// @tags: [requires_fastcount, requires_fcv_50]

t = db.update_invalid1;
t.drop();

t.update({_id: 5}, {$set: {$inc: {x: 5}}}, true);
var isDotsAndDollarsEnabled = db.adminCommand({getParameter: 1, featureFlagDotsAndDollars: 1})
                                  .featureFlagDotsAndDollars.value;
if (!isDotsAndDollarsEnabled) {
    assert.eq(0, t.count(), "A1");
} else {
    // When the dots and dollars feature flag is enabled, only top-level $-prefixed fields are
    // validated. The field '$inc' appears at a lower level than the operator $set, so it is
    // accepted by the update validation.
    assert.eq(1, t.count(), "A1");
}
