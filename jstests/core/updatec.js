
t = db.updatec;
t.drop();

t.update( { "_id" : 123 }, { $set : { "v" : { "i" : 123, "a":456 } }, $push : { "f" : 234} }, 1, 0 ); 
t.update( { "_id" : 123 }, { $set : { "v" : { "i" : 123, "a":456 } }, $push : { "f" : 234} }, 1, 0 ); 

assert.docEq(
    {
        "_id" : 123,
        "f" : [ 234, 234 ] ,
        "v" : { "i" : 123, "a" : 456 }
    } , t.findOne() );

