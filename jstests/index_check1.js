
db.somecollection.drop();

assert(db.system.namespaces.find({name:/somecollection/}).length() == 0);

db.somecollection.save({a:1});

assert(db.system.namespaces.find({name:/somecollection/}).length() == 1);

db.somecollection.ensureIndex({a:1});

assert(db.system.namespaces.find({name:/somecollection/}).length() == 2);

db.somecollection.drop();

assert(db.system.namespaces.find({name:/somecollection/}).length() == 0);

db.somecollection.save({a:1});

assert(db.system.namespaces.find({name:/somecollection/}).length() == 1);

db.somecollection.ensureIndex({a:1});

assert(db.system.namespaces.find({name:/somecollection/}).length() == 2);

assert(db.somecollection.validate().valid);
