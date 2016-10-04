// Geo mapreduce 2 from SERVER-3478

var coll = db.geoMR2;
coll.drop();

for (var i = 0; i < 300; i++)
    coll.insert({i: i, location: [10, 20]});

coll.ensureIndex({location: "2d"});

// map function
m = function() {
    emit(null, {count: this.i});
};

// reduce function
r = function(key, values) {

    var total = 0;
    for (var i = 0; i < values.length; i++) {
        total += values[i].count;
    }

    return {count: total};
};

try {
    coll.mapReduce(m, r, {
        out: coll.getName() + "_mr",
        sort: {_id: 1},
        query: {'location': {$within: {$centerSphere: [[10, 20], 0.01]}}}
    });

} catch (e) {
    // This should occur, since we can't in-mem sort for mreduce
    printjson(e);
}
