t = db.factories;
t.drop();
t.insert({name: "xyz", metro: {city: "New York", state: "NY"}});
t.ensureIndex({metro: 1});

assert(db.factories.find().count());

assert.eq(1, db.factories.find({metro: {city: "New York", state: "NY"}}).hint({metro: 1}).count());

assert.eq(1, db.factories.find({metro: {$gte: {city: "New York"}}}).hint({metro: 1}).count());
