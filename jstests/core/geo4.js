var t = db.geo4;
t.drop();

t.insert( { zip : "06525" , loc : [ 41.352964 , 73.01212  ] } );

var err = t.ensureIndex( { loc : "2d" }, { bits : 33 } );
assert.writeError(err);
assert( err.getWriteError().errmsg.indexOf("bits in geo index must be between 1 and 32") >= 0,
        tojson( err ));

assert.writeOK(t.ensureIndex( { loc : "2d" }, { bits : 32 } ));
