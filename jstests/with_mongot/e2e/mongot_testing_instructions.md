# Introduction

To run aggregation pipelines containing $search or $vectorSearch stages, you will need a mongot binary. You have the choice of running tests with a mongot binary currently running in production on Atlas (release), the latest mongot binary created via the most recent merge to 10gen/mongot repo (latest), or a mongot binary with unmerged local changes.

## Using release or latest mongot

In order to acquire a release or latest mongot binary, from your ~/mongo directory you will need to:

1. Make sure your db-contrib-tool is up-to-date. In order to do this, you will need to run:

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

<!-- TODO SERVER-86003 add instructions for downloading mongot binary from evergreen artifact/object. If packaged in mongod_binaries tarball that is pushed to s3, should be able to just use
db-contrib-tool setup-repro-env <evergreen-object-identifier> -->
