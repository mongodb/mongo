
t = db.geo_update1
t.drop()

for(var x = 0; x < 10; x++ ) { 
    for(var y = 0; y < 10; y++ ) { 
        t.insert({"loc": [x, y] , x : x , y : y , z : 1 }); 
    } 
} 

t.ensureIndex( { loc : "2d" } ) 

function p(){
    print( "--------------" );
    for ( var y=0; y<10; y++ ){
        var c = t.find( { y : y } ).sort( { x : 1 } )
        var s = "";
        while ( c.hasNext() )
            s += c.next().z + " ";
        print( s )
    }
    print( "--------------" );
}

p()

t.update({"loc" : {"$within" : {"$center" : [[5,5], 2]}}}, {'$inc' : { 'z' : 1}}, false, true); 
assert.isnull( db.getLastError() , "B1" )
p()

t.update({}, {'$inc' : { 'z' : 1}}, false, true); 
assert.isnull( db.getLastError() , "B2" )
p()


t.update({"loc" : {"$within" : {"$center" : [[5,5], 2]}}}, {'$inc' : { 'z' : 1}}, false, true); 
assert.isnull( db.getLastError() , "B3" )
p()
