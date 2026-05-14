# SERVER-53478: emit a synthetic `drop` event on `renameCollection ..., {dropTarget: true}`

## Bug

When a client issues `db.runCommand({renameCollection: src, to: dst, dropTarget: true})`
and `dst` already exists, the server records a single oplog `c` entry whose
`o` object contains `renameCollection`, `to`, and `dropTarget` (a UUID of the
overwritten target). The change-stream pipeline currently materialises only a
`rename` event from this oplog entry; the implicit drop of `dst` is invisible
to any watcher of `dst` or of the parent database.

Consumers that rely on `drop` events to invalidate downstream state
(materialised views, projection caches, search indexes) therefore silently
skip the dropped collection. The watcher of `dst` also never receives an
`invalidate`, so its cursor is left dangling against a namespace that no
longer points to the document set it was tracking.

## Proposed fix location

`src/mongo/db/pipeline/change_stream_event_transform.cpp`, in the
`kCommand` arm of `ChangeStreamEventTransformer::applyTransformation` --
specifically the branch that handles `o.renameCollection` (currently
lines 474-495). Emit a **synthetic `drop` event** for the overwritten
target whenever `o.dropTarget` is present in the same oplog entry, before
returning the rename event.

The transformer returns a single `Document` per oplog entry today. The
cleanest shape change is to make the rename branch return *two* documents
when `dropTarget` is set: the synthetic drop first, then the rename. The
caller (`DocumentSourceChangeStreamTransform::doGetNext`) already expects
to be able to buffer fan-out events (it does so today for resharding and
shard-topology change), so the plumbing exists.

The synthetic `drop` event must:

1. Carry `ns = {db, coll}` of the **overwritten target** (from `o.to`).
2. Carry `clusterTime`, `wallTime`, `lsid`, `txnNumber` identical to the
   originating rename oplog entry (so the two events share an
   `operationTime` and resume tokens sort deterministically).
3. Carry a resume token that orders strictly before the rename's resume
   token. The cleanest path: reuse the rename's `ts` and `applyOpsIndex`
   but mint a fresh `eventIdentifier` for the synthetic drop so the two
   tokens differ. The companion change in
   `src/mongo/db/pipeline/resume_token.cpp` is a one-liner: add a
   `kSyntheticDropFromRename` discriminator to the
   `eventIdentifier` enum.
4. Be gated on `!oField.getField("dropTarget").missing()` so a plain rename
   (no overwrite, `dropTarget:true` flag but no existing target) does not
   emit a spurious drop. The server only writes `o.dropTarget` when an
   existing collection was actually overwritten.

## Unified-diff sketch (do not apply -- approximate, ~30 lines)

```diff
--- a/src/mongo/db/pipeline/change_stream_event_transform.cpp
+++ b/src/mongo/db/pipeline/change_stream_event_transform.cpp
@@ -474,6 +474,29 @@ Document ChangeStreamEventTransformer::applyTransformation(...) {
             } else if (auto nssField = oField.getField("renameCollection"_sd);
                        !nssField.missing()) {
                 operationType = DocumentSourceChangeStream::kRenameCollectionOpType;

                 // The "o.renameCollection" field contains the namespace of the original collection.
                 nss = createNamespaceStringFromOplogEntry(nssField.getStringData());

                 // The "to" field contains the target namespace for the rename.
                 const auto renameTargetNss =
                     createNamespaceStringFromOplogEntry(oField["to"_sd].getStringData());
                 const auto renameTarget = makeChangeStreamNsField(renameTargetNss);

+                // SERVER-53478: if the rename overwrote an existing target collection
+                // (signalled by `o.dropTarget` carrying the overwritten target's UUID),
+                // synthesise a `drop` event for that target so watchers of the
+                // about-to-be-overwritten namespace, and database-scoped watchers, are
+                // notified before the rename lands. The synthetic event shares the
+                // rename's clusterTime/wallTime so the pair appears atomic; it sorts
+                // strictly before the rename via a `kSyntheticDropFromRename`
+                // eventIdentifier discriminator on the resume token.
+                if (!oField.getField("dropTarget"_sd).missing()) {
+                    MutableDocument syntheticDrop;
+                    syntheticDrop.addField(DocumentSourceChangeStream::kOperationTypeField,
+                                           Value(DocumentSourceChangeStream::kDropCollectionOpType));
+                    syntheticDrop.addField(DocumentSourceChangeStream::kNamespaceField,
+                                           renameTarget);
+                    syntheticDrop.addField(DocumentSourceChangeStream::kClusterTimeField,
+                                           Value(ts));
+                    syntheticDrop.addField(DocumentSourceChangeStream::kWallTimeField,
+                                           Value(wallTime));
+                    syntheticDrop.addField(DocumentSourceChangeStream::kIdField,
+                                           Value(makeResumeToken(
+                                               ts, uuid, /*eventIdentifier*/ Value(),
+                                               DocumentSourceChangeStream::kDropCollectionOpType,
+                                               ResumeTokenData::FromInvalidate::kNotFromInvalidate,
+                                               ResumeTokenData::EventIdentifierType::
+                                                   kSyntheticDropFromRename)));
+                    _bufferedSyntheticEvents.push_back(syntheticDrop.freezeToValue());
+                }
+
                 // The 'to' field predates the 'operationDescription' field which was added in 5.3.
                 // We keep the top-level 'to' field for backwards-compatibility.
                 doc.addField(DocumentSourceChangeStream::kRenameTargetNssField, renameTarget);
```

`_bufferedSyntheticEvents` is a new `std::deque<Value>` member on the
transformer; `applyTransformation` drains it before computing the next
oplog-derived event. The caller in
`DocumentSourceChangeStreamTransform::doGetNext` already loops on
transformer output, so no changes are needed there.

## Test coverage

`jstests/change_streams/rename_drop_target_emits_drop.js` (this PR)
exercises three cases: (1) collection-scoped watcher of the overwritten
target receives `drop` -> `invalidate`; (2) db-scoped watcher receives
`drop` then `rename` from the same command; (3) plain rename with no
existing target emits no spurious drop.

The unit test in `document_source_change_stream_test.cpp:1880`
(`TransformRenameShowExpandedEvents`) must be extended to assert that the
synthetic drop is the *first* document returned by the transformer when
`o.dropTarget` is present.
