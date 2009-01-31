/**
 *   Tests for the db collection 
 */



/*
 *  test drop
 */
db.getCollection( "test_db" ).drop();
assert(db.getCollection( "test_db" ).find().length() == 0);

db.getCollection( "test_db" ).save({a:1});
assert(db.getCollection( "test_db" ).find().length() == 1);

db.getCollection( "test_db" ).drop();
assert(db.getCollection( "test_db" ).find().length() == 0);

/*
 * test count
 */
 
assert(db.getCollection( "test_db" ).count() == 0);
db.getCollection( "test_db" ).save({a:1});
assert(db.getCollection( "test_db" ).count() == 1);
for (i = 0; i < 100; i++) {
    db.getCollection( "test_db" ).save({a:1});
}
assert(db.getCollection( "test_db" ).count() == 101);
db.getCollection( "test_db" ).drop();
assert(db.getCollection( "test_db" ).count() == 0);
 
/*
 *  test clean (not sure... just be sure it doen't blow up, I guess
 */ 
 
 db.getCollection( "test_db" ).clean();
 
 /*
  * test validate
  */

db.getCollection( "test_db" ).drop();
assert(db.getCollection( "test_db" ).count() == 0);

for (i = 0; i < 100; i++) {
    db.getCollection( "test_db" ).save({a:1});
}
  
var v = db.getCollection( "test_db" ).validate();
assert (v.ns == "test.test_db");
assert (v.ok == 1);

assert(v.result.toString().match(/nrecords\?:(\d+)/)[1] == 100);

/*
 * test deleteIndex, deleteIndexes
 */
 
db.getCollection( "test_db" ).drop();
assert(db.getCollection( "test_db" ).count() == 0);
db.getCollection( "test_db" ).dropIndexes();
assert(db.getCollection( "test_db" ).getIndexes().length() == 0);  

db.getCollection( "test_db" ).save({a:10});
db.getCollection( "test_db" ).ensureIndex({a:1});
db.getCollection( "test_db" ).save({a:10});

assert(db.getCollection( "test_db" ).getIndexes().length() == 1);  

db.getCollection( "test_db" ).dropIndex({a:1});
assert(db.getCollection( "test_db" ).getIndexes().length() == 0);  

db.getCollection( "test_db" ).save({a:10});
db.getCollection( "test_db" ).ensureIndex({a:1});
db.getCollection( "test_db" ).save({a:10});

assert(db.getCollection( "test_db" ).getIndexes().length() == 1);  

db.getCollection( "test_db" ).dropIndex("a_1");
assert(db.getCollection( "test_db" ).getIndexes().length() == 0);  

db.getCollection( "test_db" ).save({a:10, b:11});
db.getCollection( "test_db" ).ensureIndex({a:1});
db.getCollection( "test_db" ).ensureIndex({b:1});
db.getCollection( "test_db" ).save({a:10, b:12});

assert(db.getCollection( "test_db" ).getIndexes().length() == 2);  

db.getCollection( "test_db" ).dropIndex({b:1});
assert(db.getCollection( "test_db" ).getIndexes().length() == 1);  
db.getCollection( "test_db" ).dropIndex({a:1});
assert(db.getCollection( "test_db" ).getIndexes().length() == 0);  

db.getCollection( "test_db" ).save({a:10, b:11});
db.getCollection( "test_db" ).ensureIndex({a:1});
db.getCollection( "test_db" ).ensureIndex({b:1});
db.getCollection( "test_db" ).save({a:10, b:12});

assert(db.getCollection( "test_db" ).getIndexes().length() == 2);  

db.getCollection( "test_db" ).dropIndexes();
assert(db.getCollection( "test_db" ).getIndexes().length() == 0);  

db.getCollection( "test_db" ).find();
