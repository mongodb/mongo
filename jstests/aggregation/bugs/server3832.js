var s3832 = db.c;
s3832.drop();

s3832.save({_id: 1, a:"foo", b:"bar"});
s3832.save({_id: 2, a:"feh", b:"baz"});
s3832.save({_id: 3, a:"fee", b:"fum"});

var a1 = s3832.aggregate( { $match : { b : "baz" } } );

var a1result = [
    {
        "_id" : 2,
        "a" : "feh",
        "b" : "baz"
    }
];

assert.eq(a1.result, a1result, 's3832.a1 failed');


var a2 = s3832.aggregate( { $sort : { a : 1 } } );

var a2result = [
    {
        "_id" : 3,
        "a" : "fee",
        "b" : "fum"
    },
    {
        "_id" : 2,
        "a" : "feh",
        "b" : "baz"
    },
    {
        "_id" : 1,
        "a" : "foo",
        "b" : "bar"
    }
];

assert.eq(a2.result, a2result, 's3832.a2 failed');


var a3 = s3832.aggregate(
    { $match : { b : "baz" } },
    { $sort : { a : 1 } } );

assert.eq(a3.result, a1result, 's3832.a3 failed');


db.s3832.ensureIndex({ b : 1 }, { name : "s3832_b" });


var a4 = s3832.aggregate({ $match : { b : "baz" } });

assert.eq(a4.result, a1result, 's3832.a4 failed');


var a5 = s3832.aggregate({ $sort : { a : 1 } });

assert.eq(a5.result, a2result, 's3832.a5 failed');


var a6 = s3832.aggregate(
    { $match : { b : "baz" } },
    { $sort : { a : 1 } } );

assert.eq(a6.result, a1result, 's3832.a6 failed');


var dropb = db.s3832.dropIndex("s3832_b");

db.s3832.ensureIndex({ a : 1 }, { name : "s3832_a" });

var a7 = s3832.aggregate({ $match : { b : "baz" } });

assert.eq(a7.result, a1result, 's3832.a7 failed');


var a8 = s3832.aggregate({ $sort : { a : 1 } });

assert.eq(a8.result, a2result, 's3832.a8 failed');


var a9 = s3832.aggregate(
    { $match : { b : "baz" } },
    { $sort : { a : 1 } } );

assert.eq(a9.result, a1result, 's3832.a9 failed');
