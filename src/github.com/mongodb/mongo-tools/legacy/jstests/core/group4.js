
t = db.group4
t.drop();

function test( c , n ){
    var x = {};
    c.forEach( 
        function(z){
            assert.eq( z.count , z.values.length , n + "\t" + tojson( z ) );
        }
    );
}

t.insert({name:'bob',foo:1})
t.insert({name:'bob',foo:2})
t.insert({name:'alice',foo:1})
t.insert({name:'alice',foo:3})
t.insert({name:'fred',foo:3})
t.insert({name:'fred',foo:4})

x = t.group( 
    {
        key: {foo:1},
        initial: {count:0,values:[]},
        reduce: function (obj, prev){
            prev.count++
            prev.values.push(obj.name)
        }
    } 
);
test( x , "A" );

x = t.group(
    {
        key: {foo:1},
        initial: {count:0},
        reduce: function (obj, prev){
            if (!prev.values) {prev.values = [];}
            prev.count++;
            prev.values.push(obj.name);
        }
    }
);
test( x , "B" );

