t = db.jstests_js8;
t.drop();

t.save({a: 1, b: [2, 3, 4]});

assert.eq(1, t.find().length(), "A");
assert.eq(1,
          t.find(function() {
               return this.a == 1;
           }).length(),
          "B");
assert.eq(1,
          t.find(function() {
               if (!this.b.length)
                   return true;
               return this.b.length == 3;
           }).length(),
          "B2");
assert.eq(1,
          t.find(function() {
               return this.b[0] == 2;
           }).length(),
          "C");
assert.eq(0,
          t.find(function() {
               return this.b[0] == 3;
           }).length(),
          "D");
assert.eq(1,
          t.find(function() {
               return this.b[1] == 3;
           }).length(),
          "E");

assert(t.validate().valid);
