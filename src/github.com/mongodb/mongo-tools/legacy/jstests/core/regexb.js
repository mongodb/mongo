// Test more than four regex expressions in a query -- SERVER-969

t = db.jstests_regexb;
t.drop();

t.save( {a:'a',b:'b',c:'c',d:'d',e:'e'} );

assert.eq( 1, t.count( {a:/a/,b:/b/,c:/c/,d:/d/,e:/e/} ) );
assert.eq( 0, t.count( {a:/a/,b:/b/,c:/c/,d:/d/,e:/barf/} ) );





