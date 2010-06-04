
t = db.update_multi5;

t.drop()

t.insert({path: 'r1', subscribers: [1,2]});
t.insert({path: 'r2', subscribers: [3,4]});

t.update({}, {$addToSet: {subscribers: 5}}, false, true);

t.find().forEach(
    function(z){
        assert.eq( 3 , z.subscribers.length , z );
    }
);


