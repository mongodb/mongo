
t = db.group5;
t.drop();

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
            initial : { users : [] },
            reduce :  function(obj,prev){
                prev.users.push( obj.user );
            },
            finalize : function(x){
                x.users = x.users.length;
                return x;
            }
        });
}

assert.eq( 5 , c(0) , "g0" );
assert.eq( 10 , c(5) , "g5" );
