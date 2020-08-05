(function() {
"use strict";

writeFile(TestData.outputLocation,
          tojson(db.adminCommand("getCmdLineOpts")["parsed"]["setParameter"]));
}());
