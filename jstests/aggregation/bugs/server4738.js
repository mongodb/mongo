db = db.getSiblingDB('foo');

// test to make sure we accept all numeric types for inclusion/exclusion
var r = db.runCommand({ "aggregate" : "blah", "pipeline" : [
    { "$project" : {
        "key" : NumberLong(1),
        "v" : 1, /* javascript:  really a double */
        "x" : NumberInt(1)
    }},
    { "$group" : {
        "_id" : {
            "key" : NumberInt(1),
            "v" : 1, /* javascript:  really a double */
            "x" :NumberLong(1)
        },
        "min_v" : { "$min" : "$v" }
    }}
]});

assert(r.ok, 'support204 failed');