
t = db.mr_index3
t.drop(); 

t.insert( { _id : 1, name : 'name1', tags : ['dog', 'cat'] } ); 
t.insert( { _id : 2, name : 'name2', tags : ['cat'] } ); 
t.insert( { _id : 3, name : 'name3', tags : ['mouse', 'cat', 'dog'] } ); 
t.insert( { _id : 4, name : 'name4', tags : [] } ); 

m = function(){ 
    for ( var i=0; i<this.tags.length; i++ )
        emit( this.tags[i] , 1 )
}; 

r = function( key , values ){ 
    return Array.sum( values );
}; 

a1 = db.runCommand({ mapreduce : 'mr_index3', map : m, reduce : r , out : { inline : true } } ).results
a2 = db.runCommand({ mapreduce : 'mr_index3', map : m, reduce : r, query: {name : 'name1'} , out : { inline : true }}).results
a3 = db.runCommand({ mapreduce : 'mr_index3', map : m, reduce : r, query: {name : {$gt:'name'} } , out : { inline : true }}).results

assert.eq( [
    {
	"_id" : "cat",
	"value" : 3
    },
    {
	"_id" : "dog",
	"value" : 2
    },
    {
	"_id" : "mouse",
	"value" : 1
    }
] , a1 , "A1" );
assert.eq( [ { "_id" : "cat", "value" : 1 }, { "_id" : "dog", "value" : 1 } ] , a2 , "A2" )
assert.eq( a1 , a3 , "A3" )

t.ensureIndex({name:1, tags:1}); 

b1 = db.runCommand({ mapreduce : 'mr_index3', map : m, reduce : r , out : { inline : true } } ).results
b2 = db.runCommand({ mapreduce : 'mr_index3', map : m, reduce : r, query: {name : 'name1'} , out : { inline : true }}).results
b3 = db.runCommand({ mapreduce : 'mr_index3', map : m, reduce : r, query: {name : {$gt:'name'} } , out : { inline : true }}).results

assert.eq( a1 , b1 , "AB1" )
assert.eq( a2 , b2 , "AB2" )
assert.eq( a3 , b3 , "AB3" )


