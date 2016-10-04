
t = db.mr_stored;
t.drop();

t.save({"partner": 1, "visits": 9});
t.save({"partner": 2, "visits": 9});
t.save({"partner": 1, "visits": 11});
t.save({"partner": 1, "visits": 30});
t.save({"partner": 2, "visits": 41});
t.save({"partner": 2, "visits": 41});

m = function(obj) {
    emit(obj.partner, {stats: [obj.visits]});
};

r = function(k, v) {
    var stats = [];
    var total = 0;
    for (var i = 0; i < v.length; i++) {
        for (var j in v[i].stats) {
            stats.push(v[i].stats[j]);
            total += v[i].stats[j];
        }
    }
    return {stats: stats, total: total};
};

// Test that map reduce works with stored javascript
db.system.js.save({_id: "mr_stored_map", value: m});
db.system.js.save({_id: "mr_stored_reduce", value: r});

res = t.mapReduce(
    function() {
        mr_stored_map(this);
    },
    function(k, v) {
        return mr_stored_reduce(k, v);
    },
    {out: "mr_stored_out", scope: {xx: 1}});
// res.find().forEach( printjson )

z = res.convertToSingleObject();
assert.eq(2, Object.keySet(z).length, "A1");
assert.eq([9, 11, 30], z["1"].stats, "A2");
assert.eq([9, 41, 41], z["2"].stats, "A3");

res.drop();

m = function(obj) {
    var x = "partner";
    var y = "visits";
    emit(obj[x], {stats: [obj[y]]});
};

db.system.js.save({_id: "mr_stored_map", value: m});

res = t.mapReduce(
    function() {
        mr_stored_map(this);
    },
    function(k, v) {
        return mr_stored_reduce(k, v);
    },
    {out: "mr_stored_out", scope: {xx: 1}});
// res.find().forEach( printjson )

z = res.convertToSingleObject();
assert.eq(2, Object.keySet(z).length, "B1");
assert.eq([9, 11, 30], z["1"].stats, "B2");
assert.eq([9, 41, 41], z["2"].stats, "B3");

db.system.js.remove({_id: "mr_stored_map"});
db.system.js.remove({_id: "mr_stored_reduce"});

res.drop();
