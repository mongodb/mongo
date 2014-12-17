
t = db.group5;
t.drop();

// each group has groupnum+1 5 users
for ( var group=0; group<10; group++ ){
    for ( var i=0; i<5+group; i++ ){
        t.save( { group : "group" + group , user : i } )
    }
}

function c( group ){
    return t.group( 
        { 
            key : { group : 1 } , 
            q : { group : "group" + group } ,
            initial : { users : {} },
            reduce :  function(obj,prev){
                prev.users[obj.user] = true; // add this user to the hash
            },
            finalize : function(x){
                var count = 0;
                for (var key in x.users){
                    count++;
                }

                //replace user obj with count
                //count add new field and keep users
                x.users = count;
                return x;
            }
        })[0]; // returns array
}

assert.eq( "group0" , c(0).group , "g0" );
assert.eq( 5 ,        c(0).users , "g0 a" );
assert.eq( "group5" , c(5).group , "g5" );
assert.eq( 10 ,       c(5).users , "g5 a" );
