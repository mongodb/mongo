path = '/data/db/indexbg_dur';

m = startMongodEmpty( '--port', 30001, '--dbpath', path, '--journal', '--smallfiles', '--journalOptions', 24 );
t = m.getDB( 'test' ).test;
t.save( {x:1} );
t.createIndex( {x:1}, {background:true} );
t.count();
