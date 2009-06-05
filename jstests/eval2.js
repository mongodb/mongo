
t = db.test;
t.drop();
t.save({a:1});
t.save({a:1});

var f = db.group(
    {
        ns: "test",
        key: { a:true},
        cond: { a:1 },
        reduce: function(obj,prev) { prev.csum++; } ,
        initial: { csum: 0}
    }
);

assert(f[0].a == 1 && f[0].csum == 2 , "on db" );  

var f = t.group(
    {
        key: { a:true},
        cond: { a:1 },
        reduce: function(obj,prev) { prev.csum++; } ,
        initial: { csum: 0}
    }
);

assert(f[0].a == 1 && f[0].csum == 2 , "on coll" );  
