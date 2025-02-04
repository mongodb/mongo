// @tags: [requires_fastcount]

const t = db[jsTestName()];

function go(key) {
    t.drop();

    function check(num, name) {
        assert.eq(1, t.find().count(), tojson(key) + " count " + name);
        assert.eq(num, t.findOne().n, tojson(key) + " value " + name);
    }

    assert.commandWorked(t.update(key, {$inc: {n: 1}}, true));
    check(1, "A");

    assert.commandWorked(t.update(key, {$inc: {n: 1}}, true));
    check(2, "B");

    assert.commandWorked(t.update(key, {$inc: {n: 1}}, true));
    check(3, "C");

    let ik = {};
    for (let k in key)
        ik[k] = 1;
    assert.commandWorked(t.createIndex(ik));

    assert.commandWorked(t.update(key, {$inc: {n: 1}}, true));
    check(4, "D");
}

go({a: 5});
go({a: 5});

go({a: 5, b: 7});
go({a: null, b: 7});

go({referer: 'blah'});
go({referer: 'blah', lame: 'bar'});
go({referer: 'blah', name: 'bar'});
go({date: null, referer: 'blah', name: 'bar'});
