# SERVER-115116: Log reason for index build not being resumable at startup

## Problem

During startup recovery, when an unfinished two-phase index build is found in
the durable catalog, `catalog_repair::reconcileCatalogAndIdents` decides for
each build whether to **resume** it (via a parsed `ResumeIndexInfo` from an
internal sorter ident) or to **restart** it from scratch. Today the restart
path emits LOGV2 20660 ("Index build: restarting") but says nothing about
*why* a resume wasn't attempted. Operators investigating slow startup or
unexpected full rebuilds have to infer the reason from the absence of other
log lines.

## Decision sites (where the reason is known)

1. **Unclean shutdown** — `catalog_repair.cpp` `identHandler` returns early at
   line 71 when `lastShutdownState == kUnclean`. No resume ident is even
   inspected. Currently silent per-build.
2. **No matching resume ident on disk** — `identHandler` reaches the bottom
   without finding a `kResumableIndexIdentStem` ident for the build. Today
   inferred from "ident missing" rather than logged at the build level.
3. **Resume info failed to parse** — `readResumeIndexInfo`
   (`resumable_index_builds_common.cpp:67`) already logs 4916300, but does not
   stamp the offending `buildUUID` (the ident name is opaque to operators).
4. **Standalone mode on a repl-set node** — `startup_recovery.cpp:567` logs
   9871800 once for the whole reconcile result, but operators chasing a
   specific build can't grep by `buildUUID`.
5. **Resume attempted but threw** — `index_builds_coordinator.cpp:4841701`
   already exists and is the only case logged today per-build with reason.

## Proposed log lines

All new lines live on the storage component
(`LogComponent::kStorage`) and follow the existing
"Index build: <verb>" / structured-attr convention. Reserved IDs are in a
fresh block: **11511160–11511164**. Reason strings are an enum to keep them
queryable; the human prose lives in the message.

### 1) `11511160` — per-build "restart instead of resume" with reason

Emit from `IndexBuildsCoordinator::_restartIndexBuild` (after the existing
20660), or from `restartIndexBuildsForRecovery` immediately before the call,
whichever site has the reason in scope. Reason is one of:
`unclean_shutdown`, `no_resume_ident`, `parse_failed`, `temp_files_missing`,
`resume_setup_failed`.

```
LOGV2(11511160,
      "Index build: not resumable at startup, restarting from beginning",
      "buildUUID"_attr = buildUUID,
      "collectionUUID"_attr = collUUID,
      logAttrs(*nss),
      "reason"_attr = reason);
```

### 2) `11511161` — `identHandler` skipped resume due to unclean shutdown

Emit once per candidate ident in the unclean branch of `identHandler`
(`catalog_repair.cpp:71`). Op-debug level; helps operators correlate to a
specific stem.

```
LOGV2(11511161,
      "Index build: skipping resume after unclean shutdown",
      "ident"_attr = ident);
```

### 3) `11511162` — augment the existing parse-failure log with buildUUID

Existing LOGV2(4916300) only emits `error`. Add a sibling line stamped after
the parse fails, naming the ident so a grep yields the build directly. Keep
4916300 untouched to preserve grep history.

```
LOGV2(11511162,
      "Index build: failed to parse resume info; will restart instead of resume",
      "ident"_attr = ident,
      "error"_attr = e.toStatus());
```

### 4) `11511163` — per-build "standalone mode, neither resumed nor restarted"

The aggregate log 9871800 in `startup_recovery.cpp:567` keeps its summary
shape; emit one per-build at debug-1 immediately above it, so a build-by-build
grep is possible:

```
LOGV2(11511163,
      "Index build: not resumed and not restarted due to standalone mode",
      "buildUUID"_attr = buildUUID,
      "collectionUUID"_attr = entry.collUUID,
      "dbName"_attr = entry.dbName);
```

### 5) `11511164` — resume-info found but storage identifier missing

Already partially handled in `index_builds_coordinator.cpp:1008` via uassert
message text. Promote to a structured log emitted from `_setUpResumeIndexBuild`
prior to the uassert, with the reason `missing_storage_ident`. Lets operators
distinguish "we never tried to resume" (lines above) from "we tried and the
on-disk file vanished".

```
LOGV2(11511164,
      "Index build: cannot resume because storage identifier is missing on disk",
      "buildUUID"_attr = buildUUID,
      "collectionUUID"_attr = collUUID,
      "indexName"_attr = indexName);
```

## Reason enum (proposed IDL)

Either add to `resumable_index_builds.idl` or keep as a plain `StringData`
constant table in `index_builds_coordinator.h`. Stable values (used by the
jstest below):

| Reason                  | Source site                                          |
|-------------------------|------------------------------------------------------|
| `unclean_shutdown`      | `identHandler` (`catalog_repair.cpp`)                 |
| `no_resume_ident`       | `identHandler` reached end without `kResumableIndexIdentStem` match |
| `parse_failed`          | `readResumeIndexInfo` catch block                    |
| `temp_files_missing`    | resume-setup path, when `_tmp/<storageIdentifier>` is absent |
| `resume_setup_failed`   | `restartIndexBuildsForRecovery` catch (existing 4841701 site) |
| `standalone_mode`       | `reconcileCatalogAndRestartUnfinishedIndexBuilds`     |
| `missing_storage_ident` | `_setUpResumeIndexBuild`                              |

## Non-goals

- No behavioural change to which builds are resumed vs restarted.
- No change to existing log IDs (20660, 4916300, 4841701, 9871800, 22253).
  All new lines are additive.
- No metrics; this ticket is observability through logs only.

## Pinning

`jstests/noPassthrough/index_builds/log_reason_for_not_resuming_index_build_at_startup.js`
asserts that each reason in the enum surfaces line 11511160 with the
correct `reason` attr. Reasons reached through existing fail points
(`failToParseResumeIndexInfo`, removed `_tmp` dir) are exercised directly.
Reasons reached only through state plumbing (unclean shutdown, standalone
mode) are exercised via existing `SIGKILL` and `setReplSetMemberInStandaloneMode`
helpers.
