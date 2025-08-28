// Regression test for SERVER-13516 crash

let t = db[jsTestName()];
t.drop();

let id = NumberInt(0);
let object = {_id: id, data: []};

for (let i = 0; i < 4096; i++) {
    object.data[i] = 0;
}

t.insert(object);
t.update({_id: id}, {$pop: {data: -1}});

let modified = t.findOne();
assert.eq(4095, modified.data.length);
