(function() {
"use strict";

print("DEBUG BUILDINFO")
printjson(db.adminCommand("buildInfo"))

writeFile(TestData.outputLocation,
          tojson(db.adminCommand("getCmdLineOpts")["parsed"]["setParameter"]));
}());
