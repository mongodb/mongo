# JS Test Tags

JS Test files can leverage "tags" that suites can key off of to include and/or exclude as necessary. Not scheduling a test to run is much faster than the test doing an early-return when preconditions are not met.

The simplest use case is having something like the following at the top of your js test file:

```js
/**
 * Tests for the XYZ feature
 * @tags: [requires_fcv_81]
 */
```

These can be an array of tags:

```js
/**
 * @tags: [
 *   requires_fcv_81,
 *   requires_pipeline_optimization,
 *   not_allowed_with_signed_security_token,
 *   cannot_run_during_upgrade_downgrade,
 * ]
 */
```

and can also include (meta) comments:

```js
/**
 * @tags: [
 *   requires_fcv_81,
 *   requires_pipeline_optimization,
 *   not_allowed_with_signed_security_token,
 *   # During fcv upgrade/downgrade the engine might not be what we expect.
 *   cannot_run_during_upgrade_downgrade,
 * ]
 */
```

The tags are meant to be used in suite configurations, to [`include_with_any_tags`](../buildscripts/resmokeconfig/suites/README.md#selectorinclude_with_any_tags) and/or [`exclude_with_any_tags`](../buildscripts/resmokeconfig/suites/README.md#selectorexclude_with_any_tags):

```bash
test_kind: js_test
selector:
  include_with_any_tags:
    - multiversion_sanity_check
  exclude_with_any_tags:
    - replica_sets_multiversion_backport_required_multiversion
    - disabled_for_fcv_6_1_upgrade
```

Build variants can also use tags via the `test_flags` expansion, which facilitates tag-exclusions _across suites_ that run with the variant:

```
    expansions:
      test_flags: >-
        --excludeWithAnyTags=requires_external_data_source,requires_ldap_pool
```

## Available Tags

There is no current exhaustive list, since tags are arbitrary labels and do not need to be "registered". However, tags are always "global", and many are reused. Names should have communicate clear intent; and be reused/consolidated when appropriate.

> Use `buildscripts/resmoke.py list-tags` to find which tags are actively referenced by suite configs, although there may be more in JS files and Build Variant expansions.
