
t = db.geo_qo1;
t.drop()

t.ensureIndex({loc:"2d"}) 

t.insert({'issue':0}) 
t.insert({'issue':1}) 
t.insert({'issue':2}) 
t.insert({'issue':2, 'loc':[30.12,-118]}) 
t.insert({'issue':1, 'loc':[30.12,-118]}) 
t.insert({'issue':0, 'loc':[30.12,-118]}) 

assert.eq( 6 , t.find().itcount() , "A1" )

assert.eq( 2 , t.find({'issue':0}).itcount() , "A2" )

assert.eq( 1 , t.find({'issue':0,'loc':{$near:[30.12,-118]}}).itcount() , "A3" )

assert.eq( 2 , t.find({'issue':0}).itcount() , "B1" )

assert.eq( 6 , t.find().itcount() , "B2" )

assert.eq( 2 , t.find({'issue':0}).itcount() , "B3" )

assert.eq( 1 , t.find({'issue':0,'loc':{$near:[30.12,-118]}}).itcount() , "B4" )

