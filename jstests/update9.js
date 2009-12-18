
t = db.update9;
t.drop()

orig = { "_id" : 1 , 
         "question" : "a", 
         "choices" : { "1" : { "choice" : "b" }, 
                       "0" : { "choice" : "c" } } ,
         
       }

t.save( orig );
assert.eq( orig , t.findOne() , "A" );

t.update({_id: 1, 'choices.0.votes': {$ne: 1}}, {$push: {'choices.0.votes': 1}})

orig.choices["0"].votes = [ 1 ] ;
assert.eq( orig.choices["0"] , t.findOne().choices["0"] , "B" );

