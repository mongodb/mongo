(function() {
    "use strict";

    // SERVER-8246 Min/MaxKey should be comparable
    //
    // make sure that the MinKey MaxKey JS types are comparable

    function testType(t1, t2) {
        db.minmaxcmp.save({_id: t1});
        var doc = db.minmaxcmp.findOne({_id: t1});
        assert.eq(doc._id, t1, "Value for " + t1 + " did not round-trip to DB correctly");
        assert.neq(doc._id, t2, "Value for " + t1 + " should not equal " + t2);
        assert(doc._id instanceof t1, "Value for " + t1 + "should be instance of" + t1);
        assert(!(doc._id instanceof t2), "Value for " + t1 + "shouldn't be instance of" + t2);
    }
    testType(MinKey, MaxKey);
    testType(MaxKey, MinKey);
})();
