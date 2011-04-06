
t = db.index_check8
t.drop();

t.insert( { a : 1 , b : 1 , c : 1 , d : 1 , e : 1 } )
t.ensureIndex( { a : 1 , b : 1 , c : 1 } )
t.ensureIndex({ a: 1, b: 1, d: 1, e: 1 })

// this block could be added to many tests in theory...
if ((new Date()) % 10 == 0) {
    var coll = t.toString().substring(db.toString().length + 1);
    print("compacting " + coll + " before continuing testing");
    // don't check return code - false for mongos
    print("ok: " + db.runCommand({ compact: coll, dev: true }));
}

x = t.find( { a : 1 , b : 1 , d : 1 } ).sort( { e : 1 } ).explain()
assert( ! x.scanAndOrder , "A : " + tojson( x ) )

x = t.find( { a : 1 , b : 1 , c : 1 , d : 1 } ).sort( { e : 1 } ).explain()
//assert( ! x.scanAndOrder , "B : " + tojson( x ) )
