
// test recovery of JS engine after out of memory
db.system.js.save( { "_id" : "f1", "value" : function(n) {
    a = [];
    b = [];
    c = [];
    for (i = 0; i < n; i++) {
        a.push(Math.random());
        b.push(Math.random());
        c.push(Math.random());
    }
} })

db.eval("f1(10)");
try {
    db.eval("f1(100000000)");
    // exception should happen
    assert(false, "no out of mem");
} catch (exc) {
}

// JS engine should recover and allow more querying.. but doesnt work with v8
//db.eval("f1(10)");
//db.eval("f1(1000000)");

