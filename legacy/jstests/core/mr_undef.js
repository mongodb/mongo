
t = db.mr_undef
t.drop()

outname = "mr_undef_out"
out = db[outname]
out.drop()

t.insert({x : 0}) 

var m = function() { emit(this.mod, this.x); } 
var r = function(k,v) { total = 0; for(i in v) { total+= v[i]; } return total; } 

res = t.mapReduce(m, r, {out : outname } )

assert.eq( 0 , out.find( { _id : { $type : 6 } } ).itcount() , "A1" )
assert.eq( 1 , out.find( { _id : { $type : 10 } } ).itcount() , "A2" )

x = out.findOne()
assert.eq( x , out.findOne( { _id : x["_id"] } ) , "A3" )


