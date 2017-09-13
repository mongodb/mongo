(function() {
    "use strict";

    db.col.insert({data: 5});

    db.eval("print(5)");

    db.system.js.insert({_id: "foo", value: Code("db.col.drop()")});

    db.eval("print(5)");

    assert.eq(5, db.col.findOne()["data"]);
})();
