// In SERVER-10689, the $switch expression was introduced. In this file, we test the functionality
// of the expression.

(function() {
    "use strict";

    var coll = db.switch;
    coll.drop();

    // Insert an empty document so that something can flow through the pipeline.
    coll.insert({});

    // Ensure that a branch is correctly evaluated.
    var pipeline = {
        "$project": {
            "_id": 0,
            "output": {
                "$switch": {
                    "branches": [{"case": {"$eq": [1, 1]}, "then": "one is equal to one!"}],
                }
            }
        }
    };
    var res = coll.aggregate(pipeline).toArray();

    assert.eq(res.length, 1);
    assert.eq(res[0], {"output": "one is equal to one!"});

    // Ensure that the first branch which matches is chosen.
    pipeline = {
        "$project": {
            "_id": 0,
            "output": {
                "$switch": {
                    "branches": [
                        {"case": {"$eq": [1, 1]}, "then": "one is equal to one!"},
                        {"case": {"$eq": [2, 2]}, "then": "two is equal to two!"}
                    ],
                }
            }
        }
    };
    res = coll.aggregate(pipeline).toArray();

    assert.eq(res.length, 1);
    assert.eq(res[0], {"output": "one is equal to one!"});

    // Ensure that the default is chosen if no case matches.
    pipeline = {
        "$project": {
            "_id": 0,
            "output": {
                "$switch": {
                    "branches": [{"case": {"$eq": [1, 2]}, "then": "one is equal to two!"}],
                    "default": "no case matched."
                }
            }
        }
    };
    res = coll.aggregate(pipeline).toArray();

    assert.eq(res.length, 1);
    assert.eq(res[0], {"output": "no case matched."});

    // Ensure that nullish values are treated as false when they are a "case", and are null
    // otherwise.
    pipeline = {
        "$project": {
            "_id": 0,
            "output": {
                "$switch": {
                    "branches": [{"case": null, "then": "Null was true!"}],
                    "default": "No case matched."
                }
            }
        }
    };
    res = coll.aggregate(pipeline).toArray();

    assert.eq(res.length, 1);
    assert.eq(res[0], {"output": "No case matched."});

    pipeline = {
        "$project": {
            "_id": 0,
            "output": {
                "$switch": {
                    "branches": [{"case": "$missingField", "then": "Null was true!"}],
                    "default": "No case matched."
                }
            }
        }
    };
    res = coll.aggregate(pipeline).toArray();

    assert.eq(res.length, 1);
    assert.eq(res[0], {"output": "No case matched."});

    pipeline = {
        "$project": {
            "_id": 0,
            "output": {"$switch": {"branches": [{"case": true, "then": null}], "default": false}}
        }
    };
    res = coll.aggregate(pipeline).toArray();

    assert.eq(res.length, 1);
    assert.eq(res[0], {"output": null});

    pipeline = {
        "$project": {
            "_id": 0,
            "output": {
                "$switch":
                    {"branches": [{"case": true, "then": "$missingField"}], "default": false}
            }
        }
    };
    res = coll.aggregate(pipeline).toArray();

    assert.eq(res.length, 1);
    assert.eq(res[0], {});

    pipeline = {
        "$project": {
            "_id": 0,
            "output": {"$switch": {"branches": [{"case": null, "then": false}], "default": null}}
        }
    };
    res = coll.aggregate(pipeline).toArray();

    assert.eq(res.length, 1);
    assert.eq(res[0], {"output": null});

    pipeline = {
        "$project": {
            "_id": 0,
            "output": {
                "$switch":
                    {"branches": [{"case": null, "then": false}], "default": "$missingField"}
            }
        }
    };
    res = coll.aggregate(pipeline).toArray();

    assert.eq(res.length, 1);
    assert.eq(res[0], {});
}());
