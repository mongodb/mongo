// testing closealldatabases concurrency
// this is also a test of recoverFromYield() as that will get exercised by the update

function f() {

    // we'll use two connections to make a little parallelism
    var db2 = connect("localhost/closealltest");
    var db1 = db.getSisterDB("closealltest");

    for( var i = 0; i < 1000; i++ ) {
	db1.foo.insert({x:1}); // this does wait for a return code so we will get some parallelism
	if( i % 7 == 0 )
	    db1.foo.insert({x:99, y:2});
	if( i % 49 == 0 )
	    db1.foo.update({x:99},{a:1,b:2,c:3,d:4});
	if( i == 800 )
	    db1.foo.ensureIndex({x:1});
	var res = db2.adminCommand("closeAllDatabases");
	assert( res.ok, "closeAllDatabases res.ok=false");
    }

}

f();
print("SUCCESS closeall.js");
