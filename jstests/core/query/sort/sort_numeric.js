let t = db.sort_numeric;
t.drop();

// there are two numeric types int he db; make sure it handles them right
// for comparisons.

t.save({a: 3});
t.save({a: 3.1});
t.save({a: 2.9});
t.save({a: 1});
t.save({a: 1.9});
t.save({a: 5});
t.save({a: 4.9});
t.save({a: 2.91});

for (let pass = 0; pass < 2; pass++) {
    let c = t.find().sort({a: 1});
    let last = 0;
    while (c.hasNext()) {
        let current = c.next();
        assert(current.a > last);
        last = current.a;
    }

    assert(t.find({a: 3}).count() == 1);
    assert(t.find({a: 3.0}).count() == 1);
    assert(t.find({a: 3.0}).length() == 1);

    t.createIndex({a: 1});
}

assert(t.validate().valid);
