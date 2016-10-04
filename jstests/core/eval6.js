
t = db.eval6;
t.drop();

t.save({a: 1});

db.eval(function() {
    o = db.eval6.findOne();
    o.b = 2;
    db.eval6.save(o);
});

assert.eq(2, t.findOne().b);
