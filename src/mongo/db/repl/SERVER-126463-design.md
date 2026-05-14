# SERVER-126463 — Collection cloner `function_ref` lifetime audit

## Symptom

`CollectionCloner::insertDocumentsCallback` constructs a
`CollectionBulkLoader::ParseRecordIdAndDocFunc` (alias for
`function_ref<std::pair<RecordId, BSONObj>(const BSONObj&)>`) from one of two
captureless lambda temporaries, then forwards it into
`_collLoader->insertDocuments(docs, fn)`. The original report flags this as the
same lifetime hazard that bit SERVER-121669: a `function_ref` is a non-owning
view over a callable. Once the source temporary is destroyed, the
`function_ref` is dangling, and any later invocation dereferences freed memory.

## Root cause

`mongo::function_ref<Sig>` (`src/mongo/util/functional.h`) is modeled on the
WG21 P0792 proposal. Two relevant properties:

1. It stores a type-erased pointer-to-callable plus a thunk; it does not
   participate in the lifetime of the callable.
2. The conversion constructor `function_ref(F&&)` binds to the operand by
   address — for an rvalue lambda the binding survives only until the end of
   the full-expression that created it.

In `collection_cloner.cpp` (HEAD `266ab6d`, around lines 565–569):

```cpp
CollectionBulkLoader::ParseRecordIdAndDocFunc fn = (_recordIdsReplicated)
    ? ([](const BSONObj& doc) {
          return std::make_pair(RecordId(doc["r"].Long()), doc["d"].Obj());
      })
    : ([](const BSONObj& doc) { return std::make_pair(RecordId(0), doc); });
// The insert must be done within the lock, because CollectionBulkLoader is not
// thread safe.
uassertStatusOK(_collLoader->insertDocuments(docs, fn));
```

The two lambdas on the RHS of the ternary are temporaries of distinct unnamed
types. After narrow-contract conversion to `function_ref`, both temporaries
are destroyed at the end of the initialization full-expression. `fn` is then a
`function_ref` whose held pointer references a destroyed object. The
subsequent call `_collLoader->insertDocuments(docs, fn)` deferences `fn` once
per row in `CollectionBulkLoaderImpl::insertDocuments` (line 210:
`const auto& [replRid, doc] = fn(*insertIter++);`), each call going through
the thunk to invoke `operator()` on freed storage.

In practice the issue is latent only because both lambdas are captureless: they
decay to function pointers, and `function_ref`'s function-pointer overload
stores the address of the function, not the lambda object. The `this` pointer
in the call operator is never read, so the dereference is "paper UB" today.
A future maintainer adding a capture (e.g., a tenant marker, an `_opCtx`,
metrics counters) would convert this to live UB the moment the change lands.

## Fix proposal

Two viable fixes, listed in order of preference:

**(A) Bind the lambdas to `auto` locals.** Promote the temporaries to named
locals whose scope brackets the `insertDocuments` call. The `function_ref`
parameter then binds to a live object for the full duration of the synchronous
call:

```cpp
auto parseWithReplicatedRid = [](const BSONObj& doc) {
    return std::make_pair(RecordId(doc["r"].Long()), doc["d"].Obj());
};
auto parseWithDefaultRid = [](const BSONObj& doc) {
    return std::make_pair(RecordId(0), doc);
};
CollectionBulkLoader::ParseRecordIdAndDocFunc fn = _recordIdsReplicated
    ? CollectionBulkLoader::ParseRecordIdAndDocFunc(parseWithReplicatedRid)
    : CollectionBulkLoader::ParseRecordIdAndDocFunc(parseWithDefaultRid);
uassertStatusOK(_collLoader->insertDocuments(docs, fn));
```

This is minimally invasive and preserves the existing `function_ref` typedef.
It is also the pattern the SERVER-121669 reporter landed on in the original
escape.

**(B) Force lambda-to-function-pointer decay at the call site.** Apply unary
`+` so the operand is a function pointer rather than an unnamed lambda type:

```cpp
CollectionBulkLoader::ParseRecordIdAndDocFunc fn = _recordIdsReplicated
    ? +[](const BSONObj& doc) {
          return std::make_pair(RecordId(doc["r"].Long()), doc["d"].Obj());
      }
    : +[](const BSONObj& doc) { return std::make_pair(RecordId(0), doc); };
```

`function_ref`'s function-pointer constructor stores the pointer by value, so
the lifetime hazard disappears even though the binding remains a temporary.
This works only as long as the lambdas remain captureless; if a future change
adds captures, the `+` will fail to compile, which is a useful tripwire.

A separate follow-up — call out the type as a reference type — is tracked in
SERVER-121669: either rename `ParseRecordIdAndDocFunc` to a `…Ref` suffix or
move the `function_ref<>` wrapper from the typedef into its usages. Both are
out of scope for the targeted fix here.

## Why this is hard to reproduce in jstests

The bug is a lifetime violation in the synchronous call window inside
`insertDocumentsCallback`. The dangling `function_ref` is invoked many times
in a tight loop, but only inside one stack frame. Heap layout choices keep
the freed storage readable for the duration of the loop, so on standard
release builds the secondary completes initial sync without crashing.

The regression pin needs to (a) exercise this code path repeatedly across
realistic batch sizes, and (b) be useful when run under ASAN/UBSAN sanitisers
that detect dereferences of out-of-lifetime objects. The accompanying jstest
`jstests/noPassthrough/repl/initial_sync_cloner_fn_ref_lifetime.js` provides
that coverage by running initial sync over fault-injected source data with
both `recordIdsReplicated: true` and the default, asserting clean completion.
