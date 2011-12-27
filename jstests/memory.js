
// test creating many collections to make sure no internal cache goes OOM
for (var i = 0; i < 10000; ++i) {
    name = "foo" + i;
    if ((i % 1000) == 0) print("Processing " + name);
    db.eval(function(col) { for (var i = 0; i < 100; ++i) {db[col + "_" + i].find();} }, name);
}

// JS engine should recover and allow more querying
// but doesnt work with v8, all calls fail afterwards even form a different context
if ( typeof _threadInject == "undefined" ) {

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
assert.throws(function() { db.eval("f1(100000000)"); } );

db.eval("f1(10)");
db.eval("f1(1000000)");

}
