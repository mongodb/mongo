
t = db.index_check5
t.drop();

t.save( { "name" : "Player1" , 
          "scores" : [{"level" : 1 , "score" : 100},
                      {"level" : 2 , "score" : 50}], 
          "total" : 150 } );
t.save( { "name" : "Player2" , 
          "total" : 90 , 
          "scores" : [ {"level" : 1 , "score" : 90},
                       {"level" : 2 , "score" : 0} ]
        }  );

assert.eq( 2 , t.find( { "scores.level": 2, "scores.score": {$gt:30} } ).itcount() , "A" );
t.ensureIndex( { "scores.level" : 1 , "scores.score" : 1 } );
assert.eq( 2 , t.find( { "scores.level": 2, "scores.score": {$gt:30} } ).itcount() , "B" );
