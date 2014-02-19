
t = db.update_mulit4;
t.drop();

for(i=0;i<1000;i++){ 
    t.insert( { _id:i , 
                k:i%12,
                v:"v"+i%12 } ); 
} 

t.ensureIndex({k:1}) 

assert.eq( 84 , t.count({k:2,v:"v2"} ) , "A0" );

t.update({k:2},{$set:{v:"two v2"}},false,true) 

assert.eq( 0 , t.count({k:2,v:"v2"} ) , "A1" );
assert.eq( 84 , t.count({k:2,v:"two v2"} ) , "A2" );
