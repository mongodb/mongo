# Third Party Software Vendoring Policy

This README explains the process of vendoring third-party libraries into the MongoDB server repository.

This policy applies to [github.com/mongodb/mongo](https://github.com/mongodb/mongo).

## Adding a new third-party library to the server

1. Fork the third-party library into [github.com/mongodb-forks](https://github.com/mongodb-forks).
    > **Note:** To track versions for vulnerabilities, forking a named version (e.g., `v2.0.1`) is required against forking a specific commit.
2. Pull the library from [github.com/mongodb-forks](https://github.com/mongodb-forks) into the `src/third_party` directory inside a folder named for the library being vendored.
3. It is not necessary to update the `/sbom.json` file, as an automated Evergreen task will add the component to the SBOM once merged.
    > **Optional, but preferred:** Add component metadata to the `buildscripts/sbom/metadata.cdx.json`, see the [SBOM](#sbom) section below for field definitions. If not added, the automated SBOM generation will instead gather all available information from the C/C++ SCA tooling.
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

The `sbom.json` file in the root of the MongoDB repository contains key information about all the third party libraries that we use. It uses the CycloneDX 1.5 format.

Exhaustive documentation can be found at [https://cyclonedx.org/schema/](https://cyclonedx.org/schema/), this README is intended to describe our most common uses of fields. If your library does not easily fit the standard values below, please reach out to the Server Security team for assistance.

Custom or enriched component metadata can optionally be added to `buildscripts/sbom/metadata.cdx.json`. The automated SBOM generation Evergreen task will check for component metadata in this file and merge it with results from the C/C++ SCA tooling.

## Components

The top-level key "components" contains an array of third party components vendored in our repository. `component` objects have the following fields in `buildscripts/sbom/metadata.cdx.json`:

| Field Name | Description |
| --- | --- |
| `type` | The type of the component, such as library, application, framework, etc. For our vendored components, this will generally be `library`.|
|`bom-ref` | Should be the same as the `purl` field, including the `{{VERSION}}` as a placeholder string.|
|`supplier` and/or `author` | The entity supplying the package and/or the author(s) of the package. Must have at least one of these fields. |
|`group` | The grouping name or identifier. Typically the GitHub organization, the source package, or domain name.|
|`name` | The name of the component.|
|`version` | The version of the component. Set to `{{VERSION}}` as a placeholder string. The `import.sh` file created for the component should have a line like `VERSION=1.2.3` where the right side of the `=` specifies teh version.|
|`description` | A brief description of the package and its function.|
|`scope` | Set to `required` if package is always included in the distribution, `optional` if sometimes included (e.g., Windows-only), or `excluded` if only used from build/test/dev.
|`licenses` | Information about the licenses under which the component is used. For boilerplate licenses, this is the [SPDX license identifier](https://spdx.org/licenses/) for the license. This field also supports urls and text blobs.|
|`copyright` | A copyright notice informing users of the underlying claims to copyright ownership in a published work.|
|`cpe` and/or `purl` | The Common Platform Enumeration (CPE) [https://nvd.nist.gov/products/cpe](CPE Dictionary) and/or Package URL (PURL) [https://github.com/package-url/purl-spec](specification). It is required that one or both of these fields be populated for the purposes of SBOM vulnerability analysis. Use `{{VERSION}}` as a placeholder string.|
| `externalReferences` | This contains an array informational links about the component, typically the location of the git repo (`url`) and the type (`distribution` or `vcs`). It is used to populate [README.third_party.md](/README.third_party.md) |
| `evidence` | This contains an array of `occurrences`,  which in turn contain `location` strings specifying the location of the component in our repo.|
| `properties` | Additional custom properties related to the component, see below.|

## Properties

Component objects contain a `properties` field that is used for adding our own proprietary information to the sbom. It is a key-value array, with each key being denoted by `name`, and each value being denoted by `value`. Potential keys here include the following:

| Field Name | Description |
| --- | --- |
| `emits_persisted_data` | This should be set to true if the component outputs persisted data to disk. This is important because in this case, updating the library could cause breakage due to the format of this data changing. |
| `import_script_path` | The location of the script (if it exists) used to update the library to a new version. The standard location is `src/third_party/[componentdir]/scripts/import.sh`. |

### Component Metadata Example
```
{
  "type": "library",
  "bom-ref": "pkg:github/boostorg/boost@boost-{{VERSION}}",
  "supplier": {
    "name": "The Boost Foundation",
    "url": [
      "https://www.boost.org/"
    ]
  },
  "author": "Boost Developers",
  "group": "boost",
  "name": "Boost C++ Libraries",
  "version": "{{VERSION}}",
  "description": "Super-project for modularized Boost. Boost is a repository of free, portable, peer-reviewed C++ libraries",
  "scope": "required",
  "licenses": [
    {
      "license": {
        "id": "BSL-1.0"
      }
    }
  ],
  "copyright": "Boost copyright claims are made on a per-file basis and listed as comments in source file headers",
  "cpe": "cpe:2.3:a:boost:boost:{{VERSION}}:*:*:*:*:*:*:*",
  "purl": "pkg:github/boostorg/boost@boost-{{VERSION}}",
  "externalReferences": [
    {
      "url": "https://github.com/boostorg/boost.git",
      "type": "distribution"
    }
  ],
  "evidence": {
    "occurrences": [
      {
        "location": "src/third_party/boost"
      }
    ]
  },
  "properties": [
    {
      "name": "emits_persisted_data",
      "value": "false"
    },
    {
      "name": "import_script_path",
      "value": "src/third_party/boost/scripts/import.sh"
    }
  ]
}
```