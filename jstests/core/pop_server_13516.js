// Regression test for SERVER-13516 crash

var t = db.jstests_pop_server_13516;
t.drop();

var id = NumberInt(0);
var object = {_id: id, data: []};

for (var i = 0; i < 4096; i++) {
    object.data[i] = 0;
}

t.insert(object);
t.update({_id: id}, {$pop: {data: -1}});

var modified = t.findOne();
assert.eq(4095, modified.data.length);
