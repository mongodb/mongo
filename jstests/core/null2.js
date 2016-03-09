
t = db.null2;
t.drop();

t.insert({_id: 1, a: [{b: 5}]});
t.insert({_id: 2, a: [{}]});
t.insert({_id: 3, a: []});
t.insert({_id: 4, a: [{}, {b: 5}]});
t.insert({_id: 5, a: [5, {b: 5}]});

function doQuery(query) {
    printjson(query);
    t.find(query).forEach(function(z) {
        print("\t" + tojson(z));
    });
    return t.find(query).count();
}

function getIds(query) {
    var ids = [];
    t.find(query).forEach(function(z) {
        ids.push(z._id);
    });
    return ids;
}

theQueries = [{"a.b": null}, {"a.b": {$in: [null]}}];

for (var i = 0; i < theQueries.length; i++) {
    assert.eq(2, doQuery(theQueries[i]));
    assert.eq([2, 4], getIds(theQueries[i]));
}

t.ensureIndex({"a.b": 1});

for (var i = 0; i < theQueries.length; i++) {
    assert.eq(2, doQuery(theQueries[i]));
    assert.eq([2, 4], getIds(theQueries[i]));
}
