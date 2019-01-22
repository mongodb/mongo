// This is intended to reproduce SERVER-39109. The test ensures that when a field path which is
// illegal inside the aggregation system is used in a $match that is not pushed down to the query
// system, the correct error is raised.
(function() {
    "use strict";

    const coll = db.illegal_reference_in_match;
    assert.writeOK(coll.insert({a: 1}));

    const pipeline = [
        // The limit stage prevents the planner from pushing the match into the query layer.
        {$limit: 10},

        // 'a.$c' is an illegal path in the aggregation system (though it is legal in the query
        // system). The $limit above forces this $match to run as an aggregation stage, so the path
        // will be interpreted as illegal.
        {$match: {"a.$c": 4}},

        // This inclusion-projection allows the planner to determine that the only necessary fields
        // we need to fetch from the document are "_id" (by default), "a.$c" (since we do a match
        // on it) and "dummy" since we include/rename it as part of this $project.

        // The reason we need to explicitly include a "dummy" field, rather than just including
        // "a.$c" is that, as mentioned before, a.$c is an illegal path in the aggregation system,
        // so if we use it as part of the project, the $project will fail to parse (and the
        // relevant code will not be exercised).
        {
          $project: {
              "newAndUnrelatedField": "$dummy",
          }
        }
    ];

    const err = assert.throws(() => coll.aggregate(pipeline));
    assert.eq(err.code, 16410);
})();
