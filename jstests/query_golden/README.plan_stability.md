# Introduction

The plan_stability tests record the current winning plan for a set of ~ 1K queries produced by SPM-3816. If those plans ever change, the test is expected to fail at which point a human would decide if the changed plans are for the better or for the worse.

# Running

The plan_stability test is a standard golden test:

```bash
$ buildscripts/resmoke.py run \
  --suites=query_golden_classic \
  '--mongodSetParameters={internalQueryFrameworkControl: forceClassicEngine, planRankerMode: ...}' \
  jstests/query_golden/plan_stability.js
```

# Displaying failures

## Using the standard golden test functionality

To obtain a diff that contains an individual diff fragment for each changed plan:

1. Put the following line in `$HOME/.config/git/attributes`:

```
**/plan_stability* diff=plan_stability
```

2. Edit the `~/.golden_test_config.yml` to use a customized diff command:

```yml
diffCmd: 'git -c diff.plan_stability.xfuncname=">>>pipeline" diff --unified=0 --function-context --no-index "{{expected}}" "{{actual}}"'
```

3. You can now run `buildscripts/golden_test.py diff` as usual and the output will look like this:

```diff
...
@@ -8137,7 +8137,7 @@ >>>pipeline
 {">>>pipeline": [{"$match":{"i_compound":{"$ne":15},"z_compound":{"$nin":[6,7]}}},{"$skip":12},{"$project":{"_id":0,"a_compound":1,"h_idx":1}}],
-    "winningPlan": {"stage":"PROJECTION_SIMPLE","inputStage":{"stage":"SKIP","inputStage":{"stage":"FETCH","filter":true,"inputStage":{"stage":"IXSCAN","indexName":"z_compound_1","indexBounds":{"z_compound":["[MinKey, 6.0)","(6.0, 7.0)","(7.0, MaxKey]"]}}}}},
-    "keys" :  98745,
-    "docs" :  98743,
+    "winningPlan": {"stage":"PROJECTION_SIMPLE","inputStage":{"stage":"FETCH","inputStage":{"stage":"SKIP","inputStage":{"stage":"IXSCAN","indexName":"i_compound_1_z_compound_1","indexBounds":{"i_compound":["[MinKey, 15.0)","(15.0, MaxKey]"],"z_compound":["[MinKey, 6.0)","(6.0, 7.0)","(7.0, MaxKey]"]}}}}},
+    "keys" : 100000,
+    "docs" :  98730,
     "sorts":      0,
     "plans":      4,
     "rows" :  98730},

...

This provides the plan that changed, the pipeline it belonged to, and the execution counters that have changed.
```

## Using the summarization scripts

The `feature-extractor` internal repository contains a summarization script that
can be used to obtain a summary of the failed test as well as information on
the individual regressions that should be looked into. Please see `scripts/cbr/README.md`
in that repository for more information.

# Debugging failures

## Running the offending pipelines manually

1. Populate just the data and the indexes without executing the pipelines:

```bash
buildscripts/resmoke.py run \
  --suites=query_golden_classic \
  --mongodSetParameters='{internalQueryFrameworkControl: forceClassicEngine, planRankerMode: samplingCE, internalQuerySamplingBySequentialScan: True}' \
   jstests/query_golden/plan_stability.js \
   --pauseAfterPopulate
```

and wait until the script has advanced to the following log line:

```
[js_test:plan_stability] [jsTest] ----
[js_test:plan_stability] [jsTest] TestData.pauseAfterPopulate is set. Pausing indefinitely ...
[js_test:plan_stability] [jsTest] ----
```

2. Connect to `mongodb://127.0.0.1:20000` and run the offending pipeline against the `db.plan_stability` collection.

```bash
mongosh mongodb://127.0.0.1:20000
```

```javascript
pipeline = [...];
db.plan_stability.aggregate(pipeline).explain().queryPlanner.winningPlan;

db.plan_stability.aggregate(pipeline).explain().queryPlanner.rejectedPlans.sort((a,b) => b.costEstimate - a.costEstimate)[0]
```

## Is the new plan better or worse?

For the majority of the plans, it will be obvious if the new plan is better or worse because all the
execution counters would have moved in the same direction without any ambiguity.

Some plans, such as those involving $sort or $limit will sometimes change in a way that makes some
counters better while others become worse. For those queries, consider running them manually multiple times
to compare their wallclock execution times:

```javascript
pipeline = [...];
db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"});
db.plan_stability.aggregate(pipeline).explain('executionStats').executionStats.executionTimeMillis;
db.adminCommand({setParameter: 1, planRankerMode: "samplingCE"});
db.plan_stability.aggregate(pipeline).explain('executionStats').executionStats.executionTimeMillis;
```

You can also modify `collSize` in `plan_stability.js` to temporarily use a larger scale factor.

# Running comparisons across CE estimation methods

If you want to run a comparison between estimation methods `X` and `Y`:

1. If method `X` is not multi-planning, place the `jstests/query_golden/expected_files/X` for estimation method `X` in the root of `expected_files`, so that they are used as the base for the comparison;

2. Temporary remove the expected files for method `Y` from `expected_files/query_golden/expected_files/Y` so that they are not considered;

3. Run the test as described above, specifying `planRankerMode: X`;

4. Use the summarization script as described above to produce a report.

# Modifying the test

## Accepting the modified query plans

To accept the new plans, use `buildscripts/query_golden.py accept`, as with any other golden test.

## Removing individual pipelines

If a given pipeline proves flaky, that is, is flipping between one plan and another for no reason,
you can comment it out from the test with a note. Re-run the test and then run `buildscripts/golden_test.py accept`
to persist the change.
