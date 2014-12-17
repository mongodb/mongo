
t = db.geo_circle2;
t.drop();

t.ensureIndex({loc : "2d", categories:1}, {"name":"placesIdx", "min": -100, "max": 100});

t.insert({ "uid" : 368900 , "loc" : { "x" : -36 , "y" : -8} ,"categories" : [ "sports" , "hotel" , "restaurant"]});
t.insert({ "uid" : 555344 , "loc" : { "x" : 13 , "y" : 29} ,"categories" : [ "sports" , "hotel"]});
t.insert({ "uid" : 855878 , "loc" : { "x" : 38 , "y" : 30} ,"categories" : [ "sports" , "hotel"]});
t.insert({ "uid" : 917347 , "loc" : { "x" : 15 , "y" : 46} ,"categories" : [ "hotel"]});
t.insert({ "uid" : 647874 , "loc" : { "x" : 25 , "y" : 23} ,"categories" : [ "hotel" , "restaurant"]});
t.insert({ "uid" : 518482 , "loc" : { "x" : 4 , "y" : 25} ,"categories" : [ ]});
t.insert({ "uid" : 193466 , "loc" : { "x" : -39 , "y" : 22} ,"categories" : [ "sports" , "hotel"]});
t.insert({ "uid" : 622442 , "loc" : { "x" : -24 , "y" : -46} ,"categories" : [ "hotel"]});
t.insert({ "uid" : 297426 , "loc" : { "x" : 33 , "y" : -49} ,"categories" : [ "hotel"]});
t.insert({ "uid" : 528464 , "loc" : { "x" : -43 , "y" : 48} ,"categories" : [ "restaurant"]});
t.insert({ "uid" : 90579 , "loc" : { "x" : -4 , "y" : -23} ,"categories" : [ "restaurant"]});
t.insert({ "uid" : 368895 , "loc" : { "x" : -8 , "y" : 14} ,"categories" : [ "sports" ]});
t.insert({ "uid" : 355844 , "loc" : { "x" : 34 , "y" : -4} ,"categories" : [ "sports" , "hotel"]});


assert.eq( 10 , t.find({ "loc" : { "$within" : { "$center" : [ { "x" : 0 ,"y" : 0} , 50]}} } ).itcount() , "A" );
assert.eq( 6 , t.find({ "loc" : { "$within" : { "$center" : [ { "x" : 0 ,"y" : 0} , 50]}}, "categories" : "sports" } ).itcount() , "B" );

// When not a $near or $within query, geo index should not be used.  Fails if geo index is used.
assert.eq( 1 , t.find({ "loc" : { "x" : -36, "y" : -8}, "categories" : "sports" }).itcount(), "C" )
