
p = db.jstests_hint1;
p.drop();

p.save( { ts: new Date( 1 ), cls: "entry", verticals: "alleyinsider", live: true } );
p.ensureIndex( { ts: 1 } );

e = p.find( { live: true, ts: { $lt: new Date( 1234119308272 ) }, cls: "entry", verticals: " alleyinsider" } ).sort( { ts: -1 } ).hint( { ts: 1 } ).explain();
assert.eq( e.startKey.ts.getTime(), new Date( 1234119308272 ).getTime() , "A" );
assert.eq( 0 , e.endKey.ts.getTime() , "B" );
