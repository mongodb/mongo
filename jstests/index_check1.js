
db.somecollection.drop();

assert(db.system.namespaces.find({name:/somecollection/}).length() == 0, 1);

db.somecollection.save({a:1});

assert(db.system.namespaces.find({name:/somecollection/}).length() == 2, 2);

db.somecollection.ensureIndex({a:1});

var z = db.system.namespaces.find({name:/somecollection/}).length();
assert( z >= 1 , 3 );

if( z == 1 ) 
    print("warning: z==1, should only happen with alternate storage engines");

db.somecollection.drop();

assert(db.system.namespaces.find({name:/somecollection/}).length() == 0, 4);

db.somecollection.save({a:1});

assert(db.system.namespaces.find({name:/somecollection/}).length() == 2, 5);

db.somecollection.ensureIndex({a:1});

var x = db.system.namespaces.find({name:/somecollection/}).length();
assert( x == 2 || x == z, 6);

assert(db.somecollection.validate().valid, 7);
