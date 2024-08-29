# Introduction

To run aggregation pipelines containing $search or $vectorSearch stages, you will need a mongot binary. You have the choice of running tests with a mongot binary currently running in production on Atlas (release), the latest mongot binary created via the most recent merge to 10gen/mongot repo (latest), or a mongot binary with unmerged local changes.

## Using release or latest mongot

In order to acquire a release or latest mongot binary, from your ~/mongo directory you will need to:

1. Make sure your [db-contrib-tool](https://github.com/10gen/db-contrib-tool/tree/main) is up-to-date. In order to do this, you will need to run:

######

    python3 -m pipx upgrade db-contrib-tool

2. Know your virtual workstations OS and architecture. Assuming your VM is on ubuntu (the default), run `lscpu` in your terminal and inspect the first line of the response to confirm your VM's architecture.

The default behavior of setup-mongot-repro assume you want to download the latest version of mongot binary compatible with linux x86_64. In which case, if this works for your VM/testing needs, you can run:

######

    db-contrib-tool setup-mongot-repro-env --installDir build/install/bin

However, you can be more verbose and get the same result via:

######

    db-contrib-tool setup-mongot-repro-env --architecture x86_64  --installDir build/install/bin

and

######

    db-contrib-tool setup-mongot-repro-env --architecture x86_64 --platform linux --installDir build/install/bin

and even

######

    db-contrib-tool setup-mongot-repro-env latest --architecture x86_64 --platform linux --installDir build/install/bin

To install the production mongot linux x86_64 binary, you should run:

######

    db-contrib-tool setup-mongot-repro-env release  --architecture x86_64 --installDir build/install/bin

If your architecture is of type aarch64, to install the latest mongot binary, you should run:

######

    db-contrib-tool setup-mongot-repro-env --architecture aarch64 --installDir build/install/bin

If your VM is running macos, you can install the latest macos compatible mongot binary via:

######

    db-contrib-tool setup-mongot-repro-env --platform macos --installDir build/install/bin

Clearly, many options to play around with! To learn more about setup-mongot-repro-env command line options, use

######

    db-contrib-tool setup-mongot-repro-env --help

## Compiling mongot from source

If you want to need to include unmerged changes in your mongot binary, you can compile a mongot with said changes locally on your VM. You will need to:

1. **Clone the mongot repo to your VM**

######

    git clone git@github.com:10gen/mongot.git

2. **cd into your mongot repo and checkout the in-development branch you're interested in**
3. **Compile mongot**
   If your VM is linux x86_64:

######

    PLATFORM=linux_x86_64 make build.deploy.localdev

If your VM is linux aarch64:

######

    PLATFORM=linux_aarch64 make build.deploy.localdev

4. **Extract the mongot-localdev binary from the result tarball**

######

    tar -xvzf bazel-bin/deploy/mongot-localdev.tgz

5. **Move the resulting mongot binary** into the build directory that the server build system places mongod, mongos and shell binaries:

######

    mv mongot-localdev ~/mongo/build/install/bin

## Adding Tests

To create a new search integration test, add a jstest to **jstests/with_mongot/e2e**. Any tests added there can be run in **single node replica set** or **sharded cluster environment**.

**To run your test as a single node replica set:**

######

    buildscripts/resmoke.py run --suites=search_end_to_end_single_node

**To run your test as a sharded cluster:**

######

    buildscripts/resmoke.py run --suites=search_end_to_end_sharded_cluster

To note, until SERVER-86616 is completed, your test will have to follow a particular order:

1. Add all the documents you need for your test
2. Create a search index
3. Run your queries

This order is required to ensure correctness. This is due to the nature of data replication between mongot and mongod. Mongot replicates data from mongod via a $changeStream and is thus eventually consistent with mongod collection data. Currently, the testing infrastructure ensures correctness by expecting engineers do not make document changes after index creation (as dictated by above order) + by having the createSearchIndex shell helper wait until mongot confirms the requested mongot index is queryable before returning. More specifically, createSearchIndex uses the status of the search index (READY) generated from $listSearchIndexes to know that the collection data has been fully replicated and indexed. If we update documents or add documents after index creation, the status of $listSearchIndexes doesn't guarantee anything about the status of data replication and queries could return incorrect results.

## Downloading a mongot binary from an evergreen artifact

You can download the mongot binary that a specific evergreen patch or version utilized, which can be useful for trying to replicate errors.

You can download the mongot binary from any build variant that compiles mongot--i.e., variants which include the expansion `build_mongot: true` ([example](https://github.com/10gen/mongo/blob/848b5264be2d0f93d21ffe2e4058e810f8ea18f2/etc/evergreen_yml_components/variants/amazon/test_dev_master_branch_only.yml#L194)). More specifically, that includes:

- Compile variants that are depended upon by variants which run the search end to end tests, such as the variant `amazon-linux2-arm64-dynamic-compile` _(! Amazon Linux 2 arm64 Enterprise Shared Library Compile & Static Analysis)_, which is depended upon by _! Amazon Linux 2 arm64 Atlas Enterprise (all feature flags)_
- Variants that compile mongot **and** run the search end to end tests, such as: `amazon-linux2-arm64-mongot-integration-patchable` _(AL2 arm64 mongot integration tasks)_
  - Note that this will be true of any of the build variants that include `mongot` in the name, such as _Enterprise RHEL 8.0 Mongot Integration_

If you're confused about evergreen build variants, check out [Intro to Evergreen Concepts](https://docs.google.com/document/d/1kHi0YuzuRcMs1sRgXRRwy5-cSF4vasAT8lQjkg2hXCU/edit?usp=sharing).

The general format of the command is:

######

    db-contrib-tool setup-repro-env --variant <evergreen variant name> <evergreen patch id OR associated git commit hash>

Specifically, to download from the `AL2023 x86 mongot integration tasks cron only` build variant, you could run:

######

    db-contrib-tool setup-repro-env --variant amazon-linux-2023-x86-mongot-integration-cron-only 23b790a2a81767b8edbbc266043a205029867b74

By default, the download will be placed in `build/multiversion_bin/<githash_patchid OR githash>/dist_test/`, but you can also specify a location via the `--installDir` option. For example:

######

    db-contrib-tool setup-repro-env --variant amazon-linux2-arm64-dynamic-compile 23b790a2a81767b8edbbc266043a205029867b74 --installDir=build/multiversion_bin/my_variant

Will place the mongot binary in `build/multiversion_bin/my_variant/23b790a2a81767b8edbbc266043a205029867b74/dist_test/bin/mongot-localdev`

General information about the `setup-repro-env` command can be found in its [README](https://github.com/10gen/db-contrib-tool/blob/main/src/db_contrib_tool/setup_repro_env/README.md#setting-up-a-specific-mongodb-version). Note that if you want to download the mongot binary, you'll have to pass in an appropriate `--variant`: if you don't specify, a variant that makes sense for your machine's architecture will be automatically chosen for you, and will very likely will not be one of the variants that compiles mongot.

### Didn't Find What You're Looking For?

Visit [the landing page](https://github.com/10gen/mongo/blob/master/src/mongo/db/query/search/README.md) for all $search/$vectorSearch/$searchMeta related documentation for server contributors.
