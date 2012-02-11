// tests getlog as well as slow querying logging

glcol = db.getLogTest2;
glcol.drop()

contains = function(arr, func) {
    var i = arr.length;
    while (i--) {
        if (func(arr[i])) {
            return true;
        }
    }
    return false;
}

// test doesn't work when talking to mongos
if(db.isMaster().msg != "isdbgrid") {
  // run a slow query
  glcol.save({ "SENTINEL": 1 });
  glcol.findOne({ "SENTINEL": 1, "$where": function() { sleep(1000); return true; } });

  // run a slow update
  glcol.update({ "SENTINEL": 1, "$where": function() { sleep(1000); return true; } }, { "x": "x" });

  var resp = db.adminCommand({getLog:"global"});
  assert( resp.ok == 1, "error executing getLog command" );
  assert( resp.log, "no log field" );
  assert( resp.log.length > 0 , "no log lines" );

  // ensure that slow query is logged in detail
  assert( contains(resp.log, function(v) {
   print(v);
   return v.indexOf(" query ") != -1 && v.indexOf("query:") != -1 && v.indexOf("SENTINEL") != -1;
  }) );

  // same, but for update
  assert( contains(resp.log, function(v) {
   print(v);
   return v.indexOf(" update ") != -1 && v.indexOf("query:") != -1 && v.indexOf("SENTINEL") != -1;
  }) );
}
