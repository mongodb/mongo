
t = db.eval5;
t.drop();

t.save({a: 1, b: 2, c: 3});

assert.eq(3,
          db.eval(function(z) {
              return db.eval5.find().toArray()[0].c;
          }),
          "something weird A");

assert.isnull(db.eval(function(z) {
    return db.eval5.find({}, {a: 1}).toArray()[0].c;
}),
              "field spec didn't work");
