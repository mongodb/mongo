var t = db.sort5;
t.drop();

t.save({_id: 5, x: 1, y: {a: 5, b: 4}});
t.save({_id: 7, x: 2, y: {a: 7, b: 3}});
t.save({_id: 2, x: 3, y: {a: 2, b: 3}});
t.save({_id: 9, x: 4, y: {a: 9, b: 3}});

// test compound sorting

assert.eq([4, 2, 3, 1],
          t.find().sort({"y.b": 1, "y.a": -1}).map(function(z) {
              return z.x;
          }),
          "A no index");
t.ensureIndex({"y.b": 1, "y.a": -1});
assert.eq([4, 2, 3, 1],
          t.find().sort({"y.b": 1, "y.a": -1}).map(function(z) {
              return z.x;
          }),
          "A index");
assert(t.validate().valid, "A valid");

// test sorting on compound key involving _id

assert.eq([4, 2, 3, 1],
          t.find().sort({"y.b": 1, _id: -1}).map(function(z) {
              return z.x;
          }),
          "B no index");
t.ensureIndex({"y.b": 1, "_id": -1});
assert.eq([4, 2, 3, 1],
          t.find().sort({"y.b": 1, _id: -1}).map(function(z) {
              return z.x;
          }),
          "B index");
assert(t.validate().valid, "B valid");
