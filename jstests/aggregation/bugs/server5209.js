// use qa
db = db.getSiblingDB('qa');

db.aggtype.drop();
db.aggtype.insert({key: NumberInt(24), value: 17});
db.aggtype.insert({key: NumberLong(24), value: 8});
db.aggtype.insert({key: 24, value: 5});
db.aggtype.insert({key: NumberInt(42), value: 11});
db.aggtype.insert({key: NumberLong(42), value: 13});
db.aggtype.insert({key: 42, value: 6});

var at = db.aggtype.aggregate(
    {$group: {
        _id: "$key",
        s: {$sum: "$value"}
    }}
);

assert(at.result[0].s == 30, 'server5209 failed');
assert(at.result[1].s == 30, 'server5209 failed');
