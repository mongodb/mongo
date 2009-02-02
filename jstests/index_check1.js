
db.somecollection.drop();

assert(db.system.namespaces.find({name:/somecollection/}).length() == 0, 1);

db.somecollection.save({a:1});

assert(db.system.namespaces.find({name:/somecollection/}).length() == 1, 2);

db.somecollection.ensureIndex({a:1});

assert(db.system.namespaces.find({name:/somecollection/}).length() == 2, 3);

db.somecollection.drop();

assert(db.system.namespaces.find({name:/somecollection/}).length() == 0, 4);

db.somecollection.save({a:1});

assert(db.system.namespaces.find({name:/somecollection/}).length() == 1, 5);

db.somecollection.ensureIndex({a:1});

assert(db.system.namespaces.find({name:/somecollection/}).length() == 2, 6);

assert(db.somecollection.validate().valid, 7);
