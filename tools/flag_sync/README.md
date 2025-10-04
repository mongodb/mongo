# What is Flag Sync?

Flag Sync is a tool to allow engineers to quickly introduce or disable flags on remote client systems.
Currently, this is used to enable or disable certain Bazel flags across user workstations or CI hosts.

# Basic usage

## Prerequisites

Make sure you have valid AWS admin credentials for `devprod-build`. `aws configure sso` is the easiest way to do so.

## Get list of flags for a namespace

`bazel run //tools/flag_sync:client flag get "{namespace}"`

## Create a flag

`bazel run //tools/flag_sync:client flag create "{namespace}" "{flag_name}" "{flag_value}"`

## Toggle flag on/off

`bazel run //tools/flag_sync:client flag toggle-off "{namespace}" "{flag_name}"`
`bazel run //tools/flag_sync:client flag toggle-on "{namespace}" "{flag_name}"`

## Delete a flag

`bazel run //tools/flag_sync:client flag delete "{namespace}" "{flag_name}"`

## Namespaces

- `user-prod` - Namespace for flags to be synced with user workstations.
- `ci-prod` - Namespace for flags to be synced with CI hosts.

## Example

### Enable --config=local for users.

Create a new config local flag under the `user-prod` namespace:
`bazel run //tools/flag_sync:client flag create "user-prod" "local" "common --config=local"`

If the flag exists already, you can simple run:
`bazel run //tools/flag_sync:client flag toggle-on "user-prod" "local"`
