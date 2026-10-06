# FCV Constants Generation

> Part of the [FCV and Feature Flag documentation](README.md#fcv-and-feature-flags).

The FCV constants for each mongo version are not hardcoded in the code base but are dynamically
generated instead. We do this to make upgrading the FCV constants easier after every release. The
constants are generated at compile time, using the
[latest git tag](https://github.com/mongodb/mongo/tags) alongside a
[list of versions](https://github.com/mongodb/mongo/blob/96ea1942d25bfc6b2ab30779590f1b8a8c6887b5/src/mongo/util/version/releases.yml).

The git tag and `releases.yml` file are used as inputs to a
[template file](https://github.com/mongodb/mongo/blob/96ea1942d25bfc6b2ab30779590f1b8a8c6887b5/src/mongo/util/version/releases.h.tpl),
which the build infrastructure uses to generate a `releases.h` file that contains our constants.
Please see a sample `releases.h` file generated when latest is 7.0
[here](https://gist.github.com/XueruiFa/afc40c9ffe30049e61378af8724c86bc).

The logic for determining our generic FCVs is:

- Latest: The FCV in the
  [`featureCompatibilityVersions` list](https://github.com/mongodb/mongo/blob/96ea1942d25bfc6b2ab30779590f1b8a8c6887b5/src/mongo/util/version/releases.yml#L7)
  in `releases.yml` that is equal to the git tag.
- Last Continuous: The highest FCV in `featureCompatibilityVersions` that is less than latest FCV.
- Last LTS: The highest FCV in the
  [`longTermSupportReleases` list](https://github.com/mongodb/mongo/blob/96ea1942d25bfc6b2ab30779590f1b8a8c6887b5/src/mongo/util/version/releases.yml#L25)
  in `releases.yml` that is less than latest FCV.

## Branch Cut and Upgrading FCVs

Since the FCV generation logic is entirely dependent on the version specified in
`.bazelrc.target_mongo_version`, the Server Triage and Release (STAR) team will upgrade this version
on the master branch after every release. When this happens, to correctly build mongo after every
release, developers will need to pull the new git tag.

Developers may see what the current version is by checking `.bazelrc.target_mongo_version`. After
pulling the latest version changes, it will be necessary to recompile so that the new `releases.h`
file can be generated.
