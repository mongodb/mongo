
t = db.bench_test1;
t.drop();

t.insert( { _id : 1 , x : 1 } )
t.insert( { _id : 2 , x : 1 } )

ops = [
    { op : "findOne" , ns : t.getFullName() , query : { _id : 1 } } , 
    { op : "update" , ns : t.getFullName() , query : { _id : 1 } , update : { $inc : { x : 1 } } }
]

seconds = .7

benchArgs =  { ops : ops , parallel : 2 , seconds : seconds , host : db.getMongo().host };

if (jsTest.options().auth) {
    benchArgs['db'] = 'admin';
    benchArgs['username'] = jsTest.options().adminUser;
    benchArgs['password'] = jsTest.options().adminPassword;
}
res = benchRun( benchArgs );

assert.lte( seconds * res.update , t.findOne( { _id : 1 } ).x * 1.05 , "A1" )


assert.eq( 1 , t.getIndexes().length , "B1" )
benchRun( { ops :  [ { op : "createIndex" , ns : t.getFullName() , key : { x : 1 } } ] , parallel : 1 , seconds : 1 , host : db.getMongo().host } )
assert.eq( 2 , t.getIndexes().length , "B2" )
benchRun( { ops :  [ { op : "dropIndex" , ns : t.getFullName() , key : { x : 1 } } ] , parallel : 1 , seconds : 1 , host : db.getMongo().host } )
assert.soon( function(){ return t.getIndexes().length == 1; } );


