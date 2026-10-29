# Third Party Software Vendoring Policy

This README explains the process of vendoring third-party libraries into the MongoDB server repository.

This policy applies to [github.com/mongodb/mongo](https://github.com/mongodb/mongo).

## Adding a new third-party library to the server

1. Fork the third-party library into [github.com/mongodb-forks](https://github.com/mongodb-forks).
    > **Note:** To track versions for vulnerabilities, forking a named version (e.g., `v2.0.1`) is required against forking a specific commit.
2. Pull the library from [github.com/mongodb-forks](https://github.com/mongodb-forks) into the `src/third_party` directory inside a folder named for the library being vendored.
3. It is not necessary to update the `/sbom.json` file, as an automated Evergreen task will add the component to the SBOM once merged.
    > **Optional, but preferred:** Add component metadata to the `buildscripts/sbom/metadata.cdx.yaml`, see the [SBOM](#sbom) section below for field definitions. If not added, the automated SBOM generation will instead gather all available information from the C/C++ SCA tooling.
4. Include a `scripts/import.sh` script inside the vendored library.
    > **Note:** A specific reference to the forked branch in [github.com/mongodb-forks](https://github.com/mongodb-forks) must be hardcoded. This helps developers understand and replicate the process used to vendor a specific library, facilitating maintenance.
5. Include a `VERSION=XYZ` line in the `scripts/import.sh` script (here `XYZ` indicates the version of the third party library). This line will be used by the automated SBOM generation.

## Updating a third-party library in the server to a new upstream version

1. Fork the new upstream version to the repo already created in [github.com/mongodb-forks](https://github.com/mongodb-forks).
2. Pull the forked version from [github.com/mongodb-forks](https://github.com/mongodb-forks) to the vendored library in `src/third_party`.
3. Update `src/third_party/<vendored-library>/scripts/import.sh` with the exact reference used.
4. It is not necessary to update the `/sbom.json` file, as an automated Evergreen task will update the component version in the SBOM once merged.

## Modifying a third-party library in the server

1. Update the forked repo in [github.com/mongodb-forks](https://github.com/mongodb-forks).
2. Pull the updated fork to `src/third_party/<vendored-library>`.
3. Update the vendored commit hash in `src/third_party/<vendored-library>/scripts/import.sh`.

# SBOM

The `sbom.json` file in the root of the repository is a CycloneDX 1.6 SBOM that lists all
vendored third-party components that are included or optional in the MongoDB build. It is
generated automatically by a nightly Evergreen task.

To add or update component metadata (field values, version sources, dependency relationships),
see `buildscripts/sbom/README.md` (internal only).
