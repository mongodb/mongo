// SERVER-7343: allow $within without a geo index.
t = db.geo_withinquery;
t.drop();

num = 0;
for ( x=0; x<=20; x++ ){
    for ( y=0; y<=20; y++ ){
        o = { _id : num++ , loc : [ x , y ] }
        t.save( o )
    }
}

assert.eq(21 * 21 - 1, t.find({ $and: [ {loc: {$ne:[0,0]}},
                                        {loc: {$within: {$box: [[0,0], [100,100]]}}},
                                      ]}).itcount(), "UHOH!")
