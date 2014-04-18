var t = db.geo4;
t.drop();

t.insert( { zip : "06525" , loc : [ 41.352964 , 73.01212  ] } );

var err = t.ensureIndex( { loc : "2d" }, { bits : 33 } );
assert.commandFailed(err);
assert( err.errmsg.indexOf("bits in geo index must be between 1 and 32") >= 0,
        tojson( err ));

assert.commandWorked(t.ensureIndex( { loc : "2d" }, { bits : 32 } ));
