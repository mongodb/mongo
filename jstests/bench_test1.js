
t = db.bench_test1;
t.drop();

t.insert( { _id : 1 , x : 1 } )
t.insert( { _id : 2 , x : 1 } )

ops = [
    { op : "findOne" , ns : t.getFullName() , query : { _id : 1 } } , 
    { op : "update" , ns : t.getFullName() , query : { _id : 1 } , update : { $inc : { x : 1 } } }
]

seconds = .7

res = benchRun( { ops : ops , parallel : 2 , seconds : seconds , host : db.getMongo().host } )
assert.lte( seconds * res.update , t.findOne( { _id : 1 } ).x , "A1" )
