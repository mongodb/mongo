# Generic FCV references

> Part of the [FCV and Feature Flag documentation](README.md#fcv-and-feature-flags).

Sometimes, we may want to make a generic FCV reference to implement logic around upgrade/downgrade
that is not specific to a certain release version.

For these checks, we _must_ use the
[generic constants](https://github.com/mongodb/mongo/blob/e08eba28ab9ad4d54adb95e8517c9d43276e5336/src/mongo/db/server_options.h#L202-L216).
We should not be using the FCV constants like kVersion_6_0
([example of what to avoid](https://github.com/mongodb/mongo/blob/ef8bdb8d0cbd584d47c54d64c3215ae29ec1a32f/src/mongo/db/pipeline/document_source_list_catalog.cpp#L130)).
Instead, we should branch the different behavior using feature flags (see
[When to Use Feature Flags](README_feature_flags.md#when-to-use-feature-flags) and
[Feature Flag Gating](README_feature_flags.md#feature-flag-gating)). For generic cases that only
need to check if the server is currently in the middle of an upgrade/downgrade, use the
[isUpgradingOrDowngrading()](https://github.com/mongodb/mongo/blob/e08eba28ab9ad4d54adb95e8517c9d43276e5336/src/mongo/db/server_options.h#L275-L281)
helper.

Example: The server includes
[logic to check](https://github.com/mongodb/mongo/blob/d3fedc03bb3b2037bc4f2266b4cd106377c217b7/src/mongo/db/fcv_op_observer.cpp#L58-L71)
that connections from internal clients with a lower binary version are closed whenever we set a new
FCV. This logic is expected to stay across LTS releases as it is not specific to a particular
release version.

**_Using these generic constants and helpers indicates to the Catalog and Routing team that the FCV
logic should not be removed after the next LTS release._**

## Linter Rule

To avoid misuse of these generic FCV constants and to make sure all generic FCV references are
indeed meant to exist across LTS binary versions, **_a comment containing “(Generic FCV reference):”
is required within 10 lines before a generic FCV reference._** See
[this example](https://github.com/mongodb/mongo/blob/24890bbac9ee27cf3fb9a1b6bb8123ab120a1594/src/mongo/db/s/config/sharding_catalog_manager_shard_operations.cpp#L341-L347).
([SERVER-49520](https://jira.mongodb.org/browse/SERVER-49520) added a linter rule for this.)
