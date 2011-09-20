
x = 1;

shellHelper( "show", "tables;" )
shellHelper( "show", "tables" )
shellHelper( "show", "tables ;" )

setVerboseShell();
var res = db.test.remove({a:1});
var res2 = db.test.update({a:1}, {b: 1});
assert(res != undefined && res2 != undefined, "verbose shell 1")
setVerboseShell(false);
var res = db.test.remove({a:1});
assert(res == undefined, "verbose shell 2")

