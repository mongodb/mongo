# Gate the `phase` field of the FCV document on `featureFlagSymmetricFCV`

## Problem

`SERVER-119479` introduced a `phase` field on the featureCompatibilityVersion
document so that interrupted `setFCV` transitions can resume from the last
phase reached. The field is read only when `featureFlagSymmetricFCV` is on
(see `feature_compatibility_version.cpp` ~L224), and the field is correctly
cleared (`boost::none`) on the final write that publishes the new FCV.

However, the **write side** is not gated:
`FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument`
unconditionally executes `newFCVDoc.setPhase(phase)` regardless of the flag
state. Intermediate phases of a `setFCV` transition therefore persist a
`phase` string into `admin.system.version` on the primary, and the change
is replicated to every secondary in the oplog.

This has two observable consequences even though no code path crashes today:

1. External observers (drivers, monitoring agents) reading the FCV document
   on a primary or secondary see an internal transition state they should
   not be exposed to when the SymmetricFCV feature is "off".
2. Lagging secondaries running an older binary that does not know the
   `phase` field still receive the update in their oplog. Today the field is
   silently absorbed, but the surface area of partial-state leaking through
   replication should be eliminated to match the read-side gate.

## Fix

Gate the write inside `updateFeatureCompatibilityVersionDocument` so that
`setPhase` is only invoked when the cluster is permitted to use it.
Specifically, replace the unconditional

```cpp
newFCVDoc.setPhase(phase);
```

with

```cpp
if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
    gFeatureFlagSymmetricFCV.isEnabled()) {
    newFCVDoc.setPhase(phase);
}
```

`isVersionInitialized()` guards against the startup window before the FCV
snapshot is hydrated. The flag is `fcv_gated: false`, so `isEnabled()` is
safe once that snapshot exists. `ResolvedFCVTransition` already collapses
`endPhase` to `kComplete` when the flag is off, so any phase argument
passed in legacy mode is effectively a no-op once this write-side gate
lands.

## Risk

Behavior under the flag is unchanged. When the flag is off, intermediate
writes drop the `phase` field, matching the post-transition document shape
that has always been published. Resumability under the flag is unaffected
because the read path already requires the flag to be on before consulting
`fcvDoc.getPhase()`.

## Test

`jstests/multiVersion/fcv_phase_gated_by_symmetric_fcv.js` brings up a
two-node replica set with the primary on `latest` and the secondary on
`last-lts`, holds `setFCV` inside the transitional window with
`hangBeforeFinalizingFCV`, and asserts:

- when the flag is off, no `phase` field exists on either node, and no
  oplog entry targeting `admin.system.version` carries `phase`;
- when the flag is on, primary and secondary at least agree on the field;
- in both flag states the older-binary secondary is still applying the
  oplog (no `OplogApplicationFailure` on the `phase` write);
- the terminal post-transition document never has a `phase` field.
