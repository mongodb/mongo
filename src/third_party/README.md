# Third Party Software Vendoring Policy

This README explains the process of vendoring third-party libraries into the MongoDB server repository.

This policy applies to [github.com/mongodb/mongo](https://github.com/mongodb/mongo).

## Adding a new third-party library to the server

1. Fork the third-party library into [github.com/mongodb-forks](https://github.com/mongodb-forks).
    > **Note:** To track versions for vulnerabilities, forking a named version (e.g., `v2.0.1`) is required against forking a specific commit.
2. Pull the library from [github.com/mongodb-forks](https://github.com/mongodb-forks) into the `src/third_party` directory inside a folder named for the library being vendored.
3. Include the added library in `/sbom.json` under `components`. This will be verified by the linter in `buildscripts/sbom_linter.py`.
4. Include a `scripts/import.sh` script inside the vendored library.
    > **Note:** A specific reference to the forked branch in [github.com/mongodb-forks](https://github.com/mongodb-forks) must be hardcoded. This helps developers understand and replicate the process used to vendor a specific library, facilitating maintenance.
5. Include a `VERSION=XYZ` line in the `scripts/import.sh` script (here `XYZ` indicates the version of the third party library).

## Updating a third-party library in the server to a new upstream version

1. Fork the new upstream version to the repo already created in [github.com/mongodb-forks](https://github.com/mongodb-forks).
2. Pull the forked version from [github.com/mongodb-forks](https://github.com/mongodb-forks) to the vendored library in `src/third_party`.
3. Update `src/third_party/<vendored-library>/scripts/import.sh` with the exact reference used.
4. Update `/sbom.json` with the new vendored version.
    > **Note:** Remember to update both the `version` and the `purl`.

## Modifying a third-party library in the server

1. Update the forked repo in [github.com/mongodb-forks](https://github.com/mongodb-forks).
2. Pull the updated fork to `src/third_party/<vendored-library>`.
3. Update the vendored commit hash in `src/third_party/<vendored-library>/scripts/import.sh`.
