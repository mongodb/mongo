# OpenTelemetry Protocol (OTLP) Specification

[![Build Check](https://github.com/open-telemetry/opentelemetry-proto/workflows/Build%20Check/badge.svg?branch=main)](https://github.com/open-telemetry/opentelemetry-proto/actions?query=workflow%3A%22Build+Check%22+branch%3Amain)

This repository contains the [OTLP protocol specification](docs/specification.md)
and the corresponding Language Independent Interface Types ([.proto files](opentelemetry/proto)).

## Language Independent Interface Types

The proto files can be consumed as GIT submodules or copied and built directly in the consumer project.

The compiled files are published to central repositories (Maven, ...) from OpenTelemetry client libraries.

See [contribution guidelines](CONTRIBUTING.md) if you would like to make any changes.

## OTLP/JSON

See additional requirements for [OTLP/JSON wire representation here](https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/protocol/otlp.md#json-protobuf-encoding).

## Generate gRPC Client Libraries

To generate the raw gRPC client libraries, use `make gen-${LANGUAGE}`. Currently supported languages are:

* cpp
* csharp
* go
* java
* objc
* openapi (swagger)
* php
* python
* ruby

## Maturity Level

1.0.0 and newer releases from this repository may contain unstable (alpha or beta)
components as indicated by the Maturity table below.

| Component | Binary Protobuf Maturity | JSON Maturity |
| --------- |--------------- | ------------- |
| common/* | Stable | [Stable](docs/specification.md#json-protobuf-encoding) |
| resource/* | Stable | [Stable](docs/specification.md#json-protobuf-encoding) |
| metrics/\*<br>collector/metrics/* | Stable | [Stable](docs/specification.md#json-protobuf-encoding) |
| trace/\*<br>collector/trace/* | Stable | [Stable](docs/specification.md#json-protobuf-encoding) |
| logs/\*<br>collector/logs/* | Stable | [Stable](docs/specification.md#json-protobuf-encoding) |
| profiles/\*<br>collector/profiles/* | Experimental | [Experimental](docs/specification.md#json-protobuf-encoding) |

(See [maturity-matrix.yaml](https://github.com/open-telemetry/community/blob/47813530864b9fe5a5146f466a58bd2bb94edc72/maturity-matrix.yaml#L57)
for definition of maturity levels).

## Stability Definition

Components marked `Stable` provide the following guarantees:

- Field types, numbers and names will not change.
- Service names and `service` package names will not change.
- Service method names will not change. [from 1.0.0]
- Service method parameter names will not change. [from 1.0.0]
- Service method parameter types and return types will not change. [from 1.0.0]
- Service method kind (unary vs streaming) will not change.
- Names of `message`s and `enum`s will not change. [from 1.0.0]
- Numbers assigned to `enum` choices will not change.
- Names of `enum` choices will not change. [from 1.0.0]
- The location of `message`s and `enum`s, i.e. whether they are declared at the top lexical
  scope or nested inside another `message` will not change. [from 1.0.0]
- Package names and directory structure will not change. [from 1.0.0]
- `optional` and `repeated` declarators of existing fields will not change. [from 1.0.0]
- No existing symbol will be deleted.  [from 1.0.0]

Note: guarantees marked [from 1.0.0] will go into effect when this repository is tagged
with version number 1.0.0.

The following additive changes are allowed:

- Adding new fields to existing `message`s.
- Adding new `message`s or `enum`s.
- Adding new choices to existing `enum`s.
- Adding new choices to existing `oneof` fields.
- Adding new `service`s.
- Adding new `method`s to existing `service`s.

All the additive changes above must be accompanied by an explanation about how
new and old senders and receivers that implement the version of the protocol
before and after the change interoperate.

No guarantees are provided whatsoever about the stability of the code that
is generated from the .proto files by any particular code generator.

## Experiments

In some cases we are trying to experiment with different features. In this case,
we recommend using an "experimental" sub-directory instead of adding them to any
protocol version. These protocols should not be used, except for
development/testing purposes.

Another review must be conducted for experimental protocols to join the main project.
