// to run: 
//   ./mongo jstests/<this-file>

Array.prototype.contains = function(obj) {
    var i = this.length;
    while (i--) {
        if (this[i] === obj) {
            return true;
        }
    }
    return false;
}

var resp = db.adminCommand({getLog:"*"})
assert( resp.ok == 1, "error executing getLog command" );
assert( resp.names, "no names field" );
assert( resp.names.length > 0, "names array is empty" );
assert( resp.names.contains("global") , "missing global category" );
assert( !resp.names.contains("butty") , "missing butty category" );

resp = db.adminCommand({getLog:"global"})
assert( resp.ok == 1, "error executing getLog command" );
assert( resp.log, "no log field" );
assert( resp.log.length > 0 , "no log lines" );
