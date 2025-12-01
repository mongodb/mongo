# Query Feature Flags

This page offers some further recommendations and guidance about expanding the query language via a
feature flag. Please ensure you are familiar with the overall context documented in
[FCV_AND_FEATURE_FLAG_README.md][fcv_readme].

## Summary

Adding/changing MQL or serialization formats poses risk in some subtle edge cases and can be hard to
get right.

1. Please use the ExpressionContext's [VersionContext][version_context] to eliminate the possibility
   or edge case where two different calls to 'isEnabled' will return different values. TODO
   SERVER-111234 We should probably swap this out for an FCVSnapshot until we implement SPM-4227.

1. Be mindful that using the VersionContext and a snapshot does not currently prevent the FCV from
   changing mid operation. See [Addendum About Yielding](#addendum-about-yielding).

1. MQL can be replicated via the oplog via view definitions or collection validators. We need to
   take special care to (a) be sure to do the FCV/flag check while holding a global lock which would
   conflict with a global S lock to change the FCV (i.e. a mode stronger than MODE_IS), and (c)
   avoid FCV checks when applying oplog entries on secondaries (the primary has already validated).
   Please ensure language change features are guarded by an FCV-gated feature flag AND that you use
   `ExpressionContext::shouldParserIgnoreFeatureFlagCheck()` to detect and permit cases where a
   secondary is applying changes that a primary has already checked for FCV compatibility.

1. MQL is serialized across the wire in mixed-version clusters, so we should be sure to use FCV to
   guard our feature flags to remove any chance of nodes not understanding each other.

A good example following these recommendations is [inside our DocumentSources
parser][good_parse_example] which consults
`ExpressionContext::ignoreFeatureInParserOrRejectAndThrow()` to decide how to validate. Note that
this helper first consults whether we need to ignore validation as a replica secondary, and then
passes the VersionContext to the feature flag check. Finally, in absence of any known FCV, the
safest thing to do is default to the 'LastLTS' (Last Long Term Support-ed version) behavior.

## Details

### Replication

There have been historical bugs such as [SERVER-103028][SERVER-103028] where we mistakenly fail on a
secondary after the FCV is downgraded. The ticket description documents the problem scenario and
sequence of events, but the takeaway is that `opCtx->isEnforcingConstraints()` can be used to check
if we are currently applying oplog entries on a secondary. If this is the case, we should avoid
re-checking the feature flag to determine if novel syntax is permitted.

### Wire Protocol

When it comes to wire protocol changes, consider a server change like [SERVER-91281][SERVER-91281]
which wants to add a new option to the $sort stage to understand/accept a new option
`outputSortKeyMetadata: true/false`. This is challenging to roll out in the face of a rolling
upgrade/downgrade.

If you want to use this new option for optimizations (for example, `$setWindowFields` would like to
take advantage of this), you need to be careful to only do so when you know that any node (mongod)
participating in the query will understand your request. A pipeline may need to be routed across the
network in the face of sharded collections, and unknown options are typically rejected, which would
fail the query.

It is tempting to mistakenly conclude that because the router is the last node upgraded in the
upgrade cycle, the router can send new options with the confidence that all other nodes will
understand it. This is not correct, but hard to catch in tests. A query or sub-pipeline may be
routed from one mongod to another in the case of a `$lookup` or `$unionWith` operation acting as a
router on a shard to go find the base collection data. In this scenario, there is no guarantee which
mongod version is sending the request, and which mongod version is receiving the request.

For these reasons, the recommendation is to use the FCV to guard activating the new protocol/syntax.
The FCV can serve as a guarantee that all nodes are sufficiently upgraded and will understand the
new approach.

### Version Context

The VersionContext was introduced to introduce consistency for repeated checks of the same flag.
This is valuable both within a single host, but also across a cluster to ensure all nodes
participate in the same way. If all "is this enabled?" questions yield the same answer, the possible
states that need to be considered by the developer shrink. Note that as of this writing, the
VersionContext is only propagated over the wire for certain DDL operations where it is necessary for
correctness. [SERVER-109985][SERVER-109985] tracks an idea to apply this patten for all distributed
queries.

#### Reasoning about Safety of FCV checks racing with setFCV commands

You might be wondering if or how it is safe to check if a feature is enabled and then proceed with
the operation when a concurrent setFeatureCompatibilityVersion command may enter at any moment and
change the answer to that question. If you check whether the flag is enabled and then proceed on to
do things like write an oplog entry in a new format or send some newfangled syntax over the wire -
how do you know that the downstream consumers are still going to be ready?

To answer this, we provide an abbreviated transcript of developers discussing this possibility:

Parker Felix:

> Hi, I've been discussing a concern I've had around FCV transitions and v1/v2 oplog entries
> in #server-replication and wanted to move the discussion here. After adding an FCV check for
> applying v2 oplog entries, I noticed in a failure in a test that:
>
> 1. Starts a v2 eligible update
> 2. Hangs the update after checking the feature flag and deciding it can use the v2 oplog path
> 3. Starts an FCV 7.0->4.4 downgrade, disabling the feature flag on the primary and secondaries
> 4. Resumes the update which produces a v2 oplog entry
> 5. Oplog applier on the secondary hit a fatal assertion because it saw a v2 oplog entry while the
>    feature flag is disabledBased on [SERVER-91269][SERVER-91269] about feature flag checks and
>    their interaction with setFCV, the guidance from bullet point 3 is to not perform the flag
>    check while applying oplog entries on the secondary which is easy enough, but bullet point 2
>    about taking a global X or IX lock would require taking this lock for the entire duration of
>    all update operations which seems prohibitive.My larger concern is a scenario where we have a
>    long running $v:2 eligible update where we perform an FCV downgrade while the update is
>    running, downgrade some of the secondaries to a 4.4 binaries, and then when the update finally
>    completes, a v2 oplog entry is sent to a 4.4 node which cannot process it. As far as I
>    understand, we had no explicit way of handling this during the 4.4-5.0 upgrade/downgrade so we
>    may have just hand waived it away and I am not aware of this coming up during a 5.0 to 4.4
>    downgrade, but maybe there is some other mechanism that makes this scenario impossible that I
>    am not aware of. I think we have also ruled out supporting processing v2 oplog entries on
>    4.4-s8 so I wondering what we should do about this or if we are otherwise confident that this
>    could not happen in production.cc: @huayu @xuerui @ianb

Wenquin Ye:

> What's the specific concern with acquiring a global IX lock. Are we afraid that it may block or
> starve some other important user operation? There is some precedent for user operations to acquire a
> global IX lock to serialize with setFCV. For example [transactions acquire the global IX
> lock][txns_acquire_ix_lock_ref] when started. Also regarding, why we haven't seen this before,
> maybe it's just because we haven't done extensive downgrade testing until now. For instance [I
> recall finding another node crash issue that technically exists on the downgrade from 5.0 to
> 4.4][related_downgrade_crash_ref] but had never been observed.

Parker Felix:

> I guess I'm concerned about performance implications of changing what locks updates
> use but I'm not that familiar with our locking mechanisms. I'm also not sure if the update
> completing before the setFCV takes effect gives us strong enough guarantees. If setFCV has an
> associated oplog entry and oplog entries need to be processed in order, that might be sufficient
> for ensuring that there are no outstanding v2 oplog entries once we have completed the FCV
> transition to 4.4 (and can then have 4.4 binaries in the replica set).

huayu:

> to answer the second question, yes, setFCV has an associated oplog entry, and that oplog entry is
> processed [in its own batch][dedicated_fcv_batch_ref], so any oplog entries that happen before it
> on the primary will also > happen before it on the secondary.

jack.mulrow:

> Do updates not already take an IX lock? I thought all writes had to take an IX lock at some point
> to do things like safely check replication state, etc.

ianb:

> Yeah, updates should take an IX global lock, which will conflict with > the S lock that setFCV
> takes

jack.mulrow:

> Maybe we're checking FCV too early? If we check after taking the IX lock, I think this problem
> would go away

josef:

> Agree with Jack. @Parker Felix, the 1-3 interleaving you mention should not be allowed:
>
> 1. Starts a v2 eligible update
> 2. Hangs the update after checking the feature flag and deciding it can use the v2 oplog path
> 3. Starts an FCV 7.0->4.4 downgrade, disabling the feature flag on the primary and
>    secondaries"checking the feature flag and deciding it can use the v2 oplog path" has to happen after
>    acquiring the collection lock (which by extension acquires the global lock) and the lock must be
>    held for the entire duration of the write unit of work WUOW. This way there's no interleaving with
>    FCV changes.
>    Oh and, if this is a multi-update, it should do the above after every yield/resume, that is,
>    for each document it updates.

Parker Felix:

> Discussed with @ianb and I think removing the feature flag check in oplog application should be
> sufficient here. Updates take [IX collection lock][ix_lock_ref] at the top level, and we make the
> decision about which type of oplog entry to generate [in the update
> driver][update_driver_decision_ref]. The update's MODE_IX lock will conflict with the [MODE_S
> lock][MODE_S_lock_ref] taken by setFeatureCompatibilityVersion, so one will block after the other.
> In the event of a yield during the update query, we [don't persist any data from the update
> driver][no_persistence_update_driver_ref] and will check the value of the flag again after the yield
> in case it has changed. I think this should then guarantee that the secondaries will process the
> remaining v2 oplog entries before the setFCV downgrade oplog entry.

huayu:

> In your test failure, were we seeing a v2 oplog entry on the secondary after the setFCV downgrade
> oplog entry?

Parker Felix:

> As far as I can tell we were seeing it before the setFCV oplog entry which is the
> intended/expected behavior from what I understand. Its definitely before we see a
> "setFeatureCompatabilityVersion succeeded" message in the logs

ianb:

> FWIW one of
> the tests has an assertion about the order of the oplog entries to this
> effect: [v2_delta_oplog_entries_fcv.js][v2_delta_oplog_entries_fcv_dot_js]

```js
// Check that the sequence of oplog entries is right. We expect to see the following
// sequence, in ascending order by timestamp:
// 1) Set target FCV to 4.4
// 2) $v:2 update
// 3) Set FCV to 4.4 (removing the 'targetVersion')
// There may be other operations which happen in between these three, such as noop writes
// and so on, so we find the timestamps for (1), (2) and (3) and check that they are in the
// correct order.
```

huayu:

> I'm curious why the oplog applier on the secondary hit a fatal assertion then since it implies
> that we saw a v2 oplog entry after the FCV was downgraded, which would mean the primary also
> generated a v2 oplog entry after the FCV was downgraded right?
>
> but regardless I think it makes sense to remove the feature flag check in oplog applicationAlso
> for that test - if we set the targetFCV to 4.4 (meaning we're in the downgrading to 4.4 FCV), that
> should mean that the feature flag is disabled right? so a v2 update should not be possible right?

Parker Felix:

> It seems like FCV gated feature flags are disabled when the FCV transition starts
> rather than upon completion of FCV downgrade. For this test, we hang the update after it checks the
> value of the feature flag and determines it can generate a v2 oplog. While the update is hanging, we
> initiate an FCV downgrade to 4.4, disabling the feature flag on the secondaries. When we resume the
> update, it still produces a v2 oplog entry that was then failing the feature flag check in oplog
> application that I am going to remove.

huayu:

> OK thanks, I didn't realize where exactly we were hanging the update. I think I understand the
> sequence of events now:
>
> 1. We start an update which takes the global lock in IX mode and checks the feature flag, which is
>    enabled, so we determine it can generate a v2 oplog entry
> 2. We hang the update
> 3. We start an FCV downgrade to 4.4. This will transition the FCV to the downgrading to 4.4. phase
>    which means the feature flag is disabled. This also writes an oplog entry to update the FCV doc
>    to downgrading to 4.4 to the oplog
> 4. The FCV downgrade will hang waiting to acquire the global lock in S mode [here][MODE_S_lock_ref]
> 5. We resume the update which writes an v2 oplog entry to the oplog
> 6. FCV downgrade can now acquire the global lock, and at the end it transitions the FCV to 4.4 and
>    writes an oplog entry for that
> 7. So the order of oplog entries on both the primary and secondary are
>
>    i.(FCV transition to downgrading to 4.4),
>
>    ii. (v2 oplog entry),
>
>    iii. (FCV at 4.4.).
>
> When we apply the first (FCV transition to downgrading to 4.4) oplog entry on the secondary, this
> means the feature flag will already be disabled, so then when we check the feature flag when
> applying v2 oplog entry, it will uassert.
>
> So yes I think we can just remove the feature flag check in oplog application, and I don't think
> it should be possible to get a v2 oplog entry after the (FCV at 4.4) oplog entry because of the
> global lock conflict, so after that point it should be safe to swap the 5.0 binaries with the
> 4.4.-s8 binaries that don't understand v2 oplog entries

Parker Felix:

> Yeah that sequence of events all sounds right to me, and implies that we can safely swap out
> binaries once the FCV downgrade completes. Will go ahead and remove the feature flag check. Thanks
> for all the help with this (and thanks to everyone else on this thread as well)!

##### Addendum About Yielding

The above conversation details a correct implementation without considering yielding of locks. Any
query can yield locks during the course of execution, which raises the possibility that the FCV
snapshot becomes stale - yielding the lock might allow a setFCV command to proceed.

Implementers are encouraged to think about this possibility, which may be particularly problematic
when considering downgrade safety. How to deal with this will vary. In the case outlined above
concerning oplog format, the implementation was made to check the FCV immediately before doing the
oplog write, at a point where yielding could no longer come in between the check and the write. In
more general cases of wire protocol change, there may not be much you can do to succeed or recover
if the FCV changes mid-operation. If it is detectable, a transient error code may be best.

SPM-4227 should provide a solution by ensuring that a VersionContext remains in scope and prevents a
setFCV command from proceeding until all active operations are drained. Before that project, we
cannot offer any more specific advice.

<!-- Links -->

[fcv_readme]: /src/mongo/db/repl/FCV_AND_FEATURE_FLAG_README.md
[version_context]: /src/mongo/db/version_context.h
[good_parse_example]: https://github.com/mongodb/mongo/blob/8ac5a0c814a5e8a0f79825327fdf6c3aa118c0fa/src/mongo/db/pipeline/document_source.cpp#L135
[SERVER-103028]: https://jira.mongodb.org/browse/SERVER-103028
[SERVER-91281]: https://jira.mongodb.org/browse/SERVER-91281
[SERVER-109985]: https://jira.mongodb.org/browse/SERVER-109985
[SERVER-91269]: https://jira.mongodb.org/browse/SERVER-91269
[txns_acquire_ix_lock_ref]: https://github.com/mongodb/mongo/blob/965823ff377bc04ac0a4fce344aa9ab3f7e4eed0/src/mongo/db/transaction/transaction_participant.cpp#L1746-L1747
[related_downgrade_crash_ref]: https://jira.mongodb.org/browse/SERVER-103343
[dedicated_fcv_batch_ref]: https://github.com/mongodb/mongo/blob/770e79f6262294b67da4845a2872e123f7401a0b/src/mongo/db/namespace_string.cpp#L153-L162
[ix_lock_ref]: https://github.com/mongodb/mongo/blob/5fca8916aebed11980bdb11437ade6e5baa49198/src/mongo/db/query/write_ops/write_ops_exec.cpp#L753-L757
[update_driver_decision_ref]: https://github.com/mongodb/mongo/blob/da243b43b0879ff263a1d1ff68dcb204a5e40e47/src/mongo/db/update/update_driver.cpp#L296-L299

[MODE_S_lock_ref]: https://github.com/mongodb/mongo/blob/339bd22e371a069a167db2b7ede52c6a299fa55d/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L1388-L1393  
[no_persistence_update_driver_ref]: https://github.com/mongodb/mongo/blob/5fca8916aebed11980bdb11437ade6e5baa49198/src/mongo/db/exec/update_stage.cpp#L534-L546
[v2_delta_oplog_entries_fcv_dot_js]: https://github.com/mongodb/mongo/blob/da243b43b0879ff263a1d1ff68dcb204a5e40e47/jstests/multiVersion/s8/v2_delta_oplog_entries_fcv.js#L268-L274v2_delta_oplog_entries_fcv.js
