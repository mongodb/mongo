/**
 *   Tests for the db collection 
 */



/*
 *  test drop
 */
db.getCollection( "test_db" ).drop();
assert(db.getCollection( "test_db" ).find().length() == 0,1);

db.getCollection( "test_db" ).save({a:1});
assert(db.getCollection( "test_db" ).find().length() == 1,2);

db.getCollection( "test_db" ).drop();
assert(db.getCollection( "test_db" ).find().length() == 0,3);

/*
 * test count
 */
 
assert(db.getCollection( "test_db" ).count() == 0,4);
db.getCollection( "test_db" ).save({a:1});
assert(db.getCollection( "test_db" ).count() == 1,5);
for (i = 0; i < 100; i++) {
    db.getCollection( "test_db" ).save({a:1});
}
assert(db.getCollection( "test_db" ).count() == 101,6);
db.getCollection( "test_db" ).drop();
assert(db.getCollection( "test_db" ).count() == 0,7);
 
/*
 *  test clean (not sure... just be sure it doen't blow up, I guess
 */ 
 
 db.getCollection( "test_db" ).clean();
 
 /*
  * test validate
  */

db.getCollection( "test_db" ).drop();
assert(db.getCollection( "test_db" ).count() == 0,8);

for (i = 0; i < 100; i++) {
    db.getCollection( "test_db" ).save({a:1});
}
  
var v = db.getCollection( "test_db" ).validate();
if( v.ns != "test.test_db" ) { 
    print("Error: wrong ns name");
    print(tojson(v));
}
assert (v.ns == "test.test_db",9);
assert (v.ok == 1,10);

assert.eq(100,v.nrecords,11)

/*
 * test deleteIndex, deleteIndexes
 */
 
db.getCollection( "test_db" ).drop();
assert(db.getCollection( "test_db" ).count() == 0,12);
db.getCollection( "test_db" ).dropIndexes();
assert(db.getCollection( "test_db" ).getIndexes().length == 0,13);  

db.getCollection( "test_db" ).save({a:10});
assert(db.getCollection( "test_db" ).getIndexes().length == 1,14);  

db.getCollection( "test_db" ).ensureIndex({a:1});
db.getCollection( "test_db" ).save({a:10});

print( tojson( db.getCollection( "test_db" ).getIndexes() ) );
assert.eq(db.getCollection( "test_db" ).getIndexes().length , 2,15);  

db.getCollection( "test_db" ).dropIndex({a:1});
assert(db.getCollection( "test_db" ).getIndexes().length == 1,16);  

db.getCollection( "test_db" ).save({a:10});
db.getCollection( "test_db" ).ensureIndex({a:1});
db.getCollection( "test_db" ).save({a:10});

assert(db.getCollection( "test_db" ).getIndexes().length == 2,17);  

db.getCollection( "test_db" ).dropIndex("a_1");
assert.eq( db.getCollection( "test_db" ).getIndexes().length , 1,18);  

db.getCollection( "test_db" ).save({a:10, b:11});
db.getCollection( "test_db" ).ensureIndex({a:1});
db.getCollection( "test_db" ).ensureIndex({b:1});
db.getCollection( "test_db" ).save({a:10, b:12});

assert(db.getCollection( "test_db" ).getIndexes().length == 3,19);  

db.getCollection( "test_db" ).dropIndex({b:1});
assert(db.getCollection( "test_db" ).getIndexes().length == 2,20);  
db.getCollection( "test_db" ).dropIndex({a:1});
assert(db.getCollection( "test_db" ).getIndexes().length == 1,21);  

db.getCollection( "test_db" ).save({a:10, b:11});
db.getCollection( "test_db" ).ensureIndex({a:1});
db.getCollection( "test_db" ).ensureIndex({b:1});
db.getCollection( "test_db" ).save({a:10, b:12});

assert(db.getCollection( "test_db" ).getIndexes().length == 3,22);  

db.getCollection( "test_db" ).dropIndexes();
assert(db.getCollection( "test_db" ).getIndexes().length == 1,23);  

db.getCollection( "test_db" ).find();

db.getCollection( "test_db" ).drop();
assert(db.getCollection( "test_db" ).getIndexes().length == 0,24);  
