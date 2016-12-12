assert.writeOK(db.evalprep.insert({}), "db must exist for eval to succeed");
db.evalprep.drop();
assert.eq(17,
          db.eval(function() {
              return 11 + 6;
          }),
          "A");
assert.eq(17, db.eval(function(x) {
    return 10 + x;
}, 7), "B");

// check that functions in system.js work
db.system.js.insert({
    _id: "add",
    value: function(x, y) {
        return x + y;
    }
});
assert.eq(20, db.eval("this.add(15, 5);"), "C");
