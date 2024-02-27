# MongoDB Stable API

The MongoDB API is the user-visible behavior of all commands, including their parameters and reply
fields. An "API version" is a subset of the API for which we make an especially strong promise: For
any API version V, if an application declares API version V and uses only behaviors in V, and it is
deployed along with a specific version of an official driver, then it will experience no
semantically significant behavior changes resulting from server upgrades so long as the new server
supports V.

We can introduce compatible changes within an API version, and incompatible changes in new API
versions. Servers support multiple API versions at once; different applications can use different
API versions.

## Compatibility

For any API version V the following changes are prohibited, and must be introduced in a new API
version W.

-   Remove StableCommand (where StableCommand is some command in V).
-   Remove a documented StableCommand parameter.
-   Prohibit a formerly permitted StableCommand parameter value.
-   Remove a field from StableCommand's reply.
-   Change the type of a field in StableCommand's reply, or expand the set of types it may be.
-   Add a new value to a StableCommand reply field's enum-like fixed set of values, e.g. a new index
    type (unless there's an opt-in mechanism besides API version).
-   Change semantics of StableCommand in a manner that may cause existing applications to misbehave.
-   Change an error code returned in a particular error scenario, if drivers rely on the code.
-   Remove a label L from an error returned in a particular error scenario which had returned an error
    labeled with L before.
-   Prohibit any currently permitted CRUD syntax element, including but not limited to query and
    aggregation operators, aggregation stages and expressions, and CRUD operators.
-   Remove support for a BSON type, or any other BSON format change (besides adding a type).
-   Drop support for a wire protocol message type.
-   Making the authorization requirements for StableCommand more restrictive.
-   Increase hello.minWireVersion (or decrease maxWireVersion, which we won't do).

The following changes are permitted in V:

-   Add a command.
-   Add an optional command parameter.
-   Permit a formerly prohibited command parameter or parameter value.
-   Any change in an undocumented command parameter.
-   Change any aspect of internal sharding/replication/etc. protocols.
-   Add a command reply field.
-   Add a new error code (provided this does not break compatibility with existing drivers and
    applications).
-   Add a label to an error.
-   Change order of fields in reply docs and sub-docs.
-   Add a CRUD syntax element.
-   Making the authorization requirements for StableCommand less restrictive.
-   Add and dropping support for an authentication mechanism. Authenticate mechanisms may need to be
    removed due to security vulnerabilties and as such, there is no guarantee about their
    compatibility.
-   Deprecate a behavior
-   Increase hello.maxWireVersion.
-   Any change in behaviors not in V.
-   Performance changes.

### Enforcing Compatibility

To ensure new commits don’t introduce breaking changes to the current API version, we’ve written a
[compatibility checker script](https://github.com/mongodb/mongo/blob/6aaad044a819a50a690b932afeda9aa278ba0f2e/buildscripts/idl/idl_check_compatibility.py).
All commands in a stable API version must specify their inputs and outputs in IDL to be
compatibility checked (the only exception is the `explain` command whose output is not part of API
V1). As a side note, many command implementations derive from `TypedCommand`, which ensures
that the implementation uses the IDL spec.

The compatibility checker script compares IDL files from the new commit against both the base
commit and all [releases](https://github.com/mongodb/mongo/blob/10439de079b03a981ead7f5566e6f539a6f9becd/buildscripts/idl/checkout_idl_files_from_past_releases.py)
from 5.0.0 onwards. This compatibility checker script will run in evergreen patch builds
and in the commit queue. The script that evergreen runs is [here](https://github.com/mongodb/mongo/blob/4594ea6598ce28d01c5c5d76164b1cfeeba1494f/evergreen/check_idl_compat.sh).

### Running the Compatibility Checker Locally

To run the compatibility checker locally, first run

```
python buildscripts/idl/checkout_idl_files_from_past_releases.py -v idls
```

This creates subfolders of past releases in the `idls` folder. Then, for the old release you want to
check against, run

```
python buildscripts/idl/idl_check_compatibility.py -v --old-include idls/<old_release_dir>/src --old-include idls/<old_release_dir>/src/mongo/db/modules/enterprise/src --new-include src --new-include src/mongo/db/modules/enterprise/src idls/<old_release_dir>/src src
```

For example:

```
python buildscripts/idl/idl_check_compatibility.py -v --old-include idls/r6.0.3/src --old-include idls/r6.0.3/src/mongo/db/modules/enterprise/src --new-include src --new-include src/mongo/db/modules/enterprise/src idls/r6.0.3/src src
```

## Adding new commands and fields

**_Any additions to the Stable API must be approved by the Stable API PM and code reviewed by the
Query Optimization Team._**

Adding a new IDL command requires the `api_version` field, which indicates which Stable API version
this command is in. **_By default, the `api_version` field should be `""`._** Only if you are adding the
command to the Stable API, then `api_version` should be the API version you are adding it to
(currently `"1"`). **_By adding it to the Stable API, this means you cannot remove this
command within this API version._**

Adding a new command parameter or reply field requires the `stability` field. This field indicates
whether the command parameter/reply field is part of the Stable API. There are three options for
field: `unstable`, `internal`, and `stable`. If you are unsure what the `stability` field for the
new command parameter or reply field should be, it **_should be marked as `stability: unstable`_**.

Only if the field should be added to the Stable API, then you should mark the field as
`stability: stable`in IDL. Additionally, in `idl_check_compatibility.py` you must add the field to
the `ALLOWED_STABLE_FIELDS_LIST`. This list was added so that engineers are aware that by making a
field part of the stable API, **_the field cannot be changed in any way that would violate the
Stable API guidelines_** (see [above](https://github.com/mongodb/mongo/blob/master/src/mongo/db/STABLE_API_README.md#compatibility)).
Crucially, this means the field **_cannot be removed or changed to `stability: unstable` or
`stability: internal`_** while we are in the current API version.

The format of adding a field to the list is `<command_name>-<command_param_or_reply_field>-<field_name>`.

### `stability: unstable` vs. `stability: internal`

If the field should not be part of the Stable API, it should be marked as either
`stability: unstable` or `stability: internal`. Both of these mean that the field will not be a part
of the Stable API. The difference is that when we send commands from a mongos to a shard, the shard
will perform parsing validation that checks that all the command fields are part of the Stable API,
and will throw an `APIStrict` error if the field is marked as `stability: unstable`, but not if the
field is marked as `stability: internal`. `stability: internal` was added to allow us to get past
this error while still not adding the field to the Stable API. So in general, a field should be
marked as `stability: unstable`, unless it will go through this parsing validation, in which case it
should be marked as `stability: internal`.

### `IGNORE_STABLE_TO_UNSTABLE_LIST`

The `IGNORE_STABLE_TO_UNSTABLE_LIST` exists because there have been cases where a field was added
to the Stable API accidentally, and since the field was strictly internal / not documented to users,
we changed the field to be unstable. (Note that these kinds of changes have to go through the same
approval process.) Normally changing a field from `stability: stable` to `stability: unstable` or
`stability: internal` would throw an error, so the `IGNORE_STABLE_TO_UNSTABLE_LIST` acts as an allow
list for these exceptions.

**_Additions to the `IGNORE_STABLE_TO_UNSTABLE_LIST` must be approved by the Stable API PM and code
reviewed by the Query Optimization Team._**

### The BSON serialization `any` type

The `bson_serialization_type` is used to define the BSON type that an IDL field will serialize to.
In some cases, we need custom serializers defined in C++ to perform more complex logic,
such as validating the given type or accepting multiple types for the field. If we use these custom
serializers, we specify the `bson_serialization_type` to be `any`. However, the compatibility
checker script can’t type check `any` , since the main logic for the type exists outside of the
IDL file. As many commands have valid reasons for using type `any`, we do not restrict usage.
Instead, the command must be added to an [allowlist](https://github.com/mongodb/mongo/blob/6aaad044a819a50a690b932afeda9aa278ba0f2e/buildscripts/idl/idl_check_compatibility.py#L52).
This also applies to any fields marked as `stability: unstable`. This is to prevent unexpected
errors when modifying a field from `stability: unstable` to `stability: stable`. By intentionally
opting in, we assume the implementer understands the implications and has valid reasons to use
`any`.

## Stable API implementation

All `Command` subclasses implement `apiVersions()`, which returns the set of API versions the
command is part of. By default, a command is not included in any API version, meaning it has no
special backward compatibility guarantees. `Command` subclasses also implement
`deprecatedApiVersions()`, which returns the set of API versions in which the command is deprecated.
This is a subset of `apiVersions()`.

## API version parameters

All commands accept three parameters: apiVersion, apiStrict, and apiDeprecationErrors. Users
configure their drivers to add these parameters to each command invocation. These parameters express
which version of the MongoDB API the user requests, whether to permit invocations of commands that
are not in any API version, and whether to permit deprecated behaviors. If a behavior has changed
between API versions, the server uses the client's apiVersion parameter to choose how to behave. The
API version parameters are all optional, except if the server parameter requireApiVersion is true:
then all commands require apiVersion.

The default apiVersion is "1". In the future we can remove the default and make apiVersion a
required parameter for all commands. We can never change the default, however.

## Dropping API versions

To drop an API version U, we must publish at least one server release R that supports both U and
some newer API version V. It must support both versions in R's fully upgraded FCV (see below). This
gives users the opportunity to update their code to use V without downtime.

## Deprecation

We can deprecate behaviors (which include commands, command parameters, etc.) in V in any server
release R. We should deprecate a behavior if we plan to remove it in a later API version. Ideally,
if R supports API versions V and W, and a user's code runs with no deprecation errors in V on R,
then the user can upgrade to W on R with no further code changes. In some cases, however, there will
be incompatible changes in W that can't be foretold by deprecations in V.

## featureCompatibilityVersion (FCV)

Rules for feature compatibility version and API version:

### Rule 1

**The first release to support an API version W can add W in its upgraded FCV, but cannot add W in
its downgraded FCV.**

Some API versions will introduce behaviors that require disk format changes or intracluster protocol
changes that don't take effect until setFCV("R"), so for consistency, we always wait for setFCV("R")
before supporting a new API version.

### Rule 2

**So that applications can upgrade without downtime from V to W, at least one release must support
both V and W in its upgraded FCV.**

This permits zero-downtime API version upgrades. If release R in its upgraded FCV "R" supports both
V and W, the customer can first upgrade to R with FCV "R" while their application is running with
API version V, then redeploy their application with the code updated to W while the server is
running.

It isn't sufficient for a binary release to support V and W in its downgraded FCV, because cloud
users have no control over when a binary-upgraded server executes setFCV("R"). The server must
support both API versions in its upgraded FCV, because this is its steady state.
