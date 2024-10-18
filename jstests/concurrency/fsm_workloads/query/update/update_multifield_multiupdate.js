/**
 * update_multifield_multiupdate.js
 *
 * Does updates that affect multiple fields on multiple documents.
 * The collection has an index for each field, and a multikey index for all fields.
 * @tags: [
 *   # Runs a multi-update which is non-retryable.
 *   requires_non_retryable_writes
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/query/update/update_multifield.js";

export const $config = extendWorkload($baseConfig,
                                      function($config, $super) {
                                          $config.data.multi = true;

                                          $config.data.assertResult = function(
                                              res, db, collName, query) {
                                              assert.eq(0, res.nUpserted, tojson(res));

                                              if (isMongod(db)) {
                                                  // If a document's RecordId cannot change, then we
                                                  // should not have updated any document more than
                                                  // once, since the update stage internally
                                                  // de-duplicates based on RecordId.
                                                  assert.lte(
                                                      this.numDocs, res.nMatched, tojson(res));
                                              } else {  // mongos
                                                  assert.gte(res.nMatched, 0, tojson(res));
                                              }

                                              assert.eq(res.nMatched, res.nModified, tojson(res));

                                              if (TestData.runningWithBalancer !== true) {
                                                  var docs = db[collName].find().toArray();
                                                  docs.forEach(function(doc) {
                                                      assert.eq(
'number',
typeof doc.z,
`The query is ${tojson(query)}, and doc is ${
tojson(doc)}, the number of all docs is ${
docs.length}. The response of update is ${tojson(res)}, and config multi is ${
$config.data.multi.toString()}`);
                                                      assert.gt(doc.z, 0);
                                                  });
                                              }
                                          };

                                          return $config;
                                      });
