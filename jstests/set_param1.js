
old = db.adminCommand( { "getParameter" : "*" } )
tmp1 = db.adminCommand( { "setParameter" : 1 , "logLevel" : 5 } )
tmp2 = db.adminCommand( { "setParameter" : 1 , "logLevel" : old.logLevel } )
now = db.adminCommand( { "getParameter" : "*" } )

assert.eq( old , now , "A" )
assert.eq( old.logLevel , tmp1.was , "B" )
assert.eq( 5 , tmp2.was , "C" )
