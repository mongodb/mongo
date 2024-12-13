# OTLP Exporter

The [OpenTelemetry
Protocol](https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/protocol/README.md)
(OTLP) is a vendor-agnostic protocol designed as part of the OpenTelemetry
project. The OTLP exporter can be used to export to any backend that supports
OTLP.

The [OpenTelemetry
Collector](https://github.com/open-telemetry/opentelemetry-collector) is a
reference implementation of an OTLP backend. The Collector can be configured to
export to many other backends, such as Zipkin and Jaegar.

For a full list of backends supported by the Collector, see the [main Collector
repo](https://github.com/open-telemetry/opentelemetry-collector/tree/main/exporter)
and [Collector
contributions](https://github.com/open-telemetry/opentelemetry-collector-contrib/tree/main/exporter).

## Configuration

The OTLP exporter offers some configuration options. To configure the exporter,
create an `OtlpGrpcExporterOptions` struct (defined in
[otlp_grpc_exporter.h](https://github.com/open-telemetry/opentelemetry-cpp/blob/main/exporters/otlp/include/opentelemetry/exporters/otlp/otlp_grpc_exporter.h)),
set the options inside, and pass the struct to the `OtlpGrpcExporter` constructor,
like so:

```cpp
OtlpGrpcExporterOptions options;
options.endpoint = "localhost:12345";
auto exporter = std::unique_ptr<sdktrace::SpanExporter>(new otlp::OtlpGrpcExporter(options));
```

The OTLP HTTP exporter offers some configuration options. To configure the exporter,
create an `OtlpHttpExporterOptions` struct (defined in
[otlp_http_exporter.h](https://github.com/open-telemetry/opentelemetry-cpp/blob/main/exporters/otlp/include/opentelemetry/exporters/otlp/otlp_http_exporter.h)),
set the options inside, and pass the struct to the `OtlpHttpExporter` constructor,
like so:

```cpp
OtlpHttpExporterOptions options;
options.url = "localhost:12345";
auto exporter = std::unique_ptr<sdktrace::SpanExporter>(new otlp::OtlpHttpExporter(options));
```

The OTLP File exporter offers some configuration options. To configure the exporter,
create an `OtlpFileExporterOptions` struct (defined in
[otlp_file_exporter_options.h](https://github.com/open-telemetry/opentelemetry-cpp/blob/main/exporters/otlp/include/opentelemetry/exporters/otlp/otlp_file_exporter_options.h)
and
[otlp_file_client_options.h](https://github.com/open-telemetry/opentelemetry-cpp/blob/main/exporters/otlp/include/opentelemetry/exporters/otlp/otlp_file_client_options.h)),
set the options inside, and pass the struct to the `OtlpFileExporter` constructor,
like so:

```cpp
OtlpFileExporterOptions options;
OtlpFileClientFileSystemOptions fs_backend;
fs_backend.file_pattern = "trace.%N.log";
options.backend_options = fs_backend;
auto exporter = std::unique_ptr<sdktrace::SpanExporter>(new otlp::OtlpFileExporter(options));
```

### Configuration options ( OTLP GRPC Exporter )

| Option                           | Env Variable                                 | Default               | Description                          |
|----------------------------------|----------------------------------------------|-----------------------|--------------------------------------|
|`endpoint`                        |`OTEL_EXPORTER_OTLP_ENDPOINT`                 |`http://localhost:4317`| The OTLP GRPC endpoint to connect to |
|                                  |`OTEL_EXPORTER_OTLP_TRACES_ENDPOINT`          |                       |                                      |
|`use_ssl_credentials`             |`OTEL_EXPORTER_OTLP_SSL_ENABLE`               | `false`               | Whether the endpoint is SSL enabled  |
|                                  |`OTEL_EXPORTER_OTLP_TRACES_SSL_ENABLE`        |                       |                                      |
|`ssl_credentials_cacert_path`     |`OTEL_EXPORTER_OTLP_CERTIFICATE`              | `""`                  | SSL Certificate file path            |
|                                  |`OTEL_EXPORTER_OTLP_TRACES_CERTIFICATE`       |                       |                                      |
|`ssl_credentials_cacert_as_string`|`OTEL_EXPORTER_OTLP_CERTIFICATE_STRING`       | `""`                  | SSL Certifcate as in-memory string   |
|                                  |`OTEL_EXPORTER_OTLP_TRACES_CERTIFICATE_STRING`|                       |                                      |
|`timeout`                         |`OTEL_EXPORTER_OTLP_TIMEOUT`                  | `10s`                 | GRPC deadline                        |
|                                  |`OTEL_EXPORTER_OTLP_TRACES_TIMEOUT`           |                       |                                      |
|`metadata`                        |`OTEL_EXPORTER_OTLP_HEADERS`                  |                       | Custom metadata for GRPC             |
|                                  |`OTEL_EXPORTER_OTLP_TRACES_HEADERS`           |                       |                                      |

### Configuration options ( OTLP HTTP Exporter )

| Option             | Env Variable                       | Default                         | Description                                                       |
|--------------------|------------------------------------|---------------------------------|-------------------------------------------------------------------|
|`url`               |`OTEL_EXPORTER_OTLP_ENDPOINT`       |`http://localhost:4318`          | The OTLP HTTP endpoint to connect to                              |
|                    |`OTEL_EXPORTER_OTLP_TRACES_ENDPOINT`|                                 |                                                                   |
|`content_type`      | n/a                                | `application/json`              | Data format used - JSON or Binary                                 |
|`json_bytes_mapping`| n/a                                | `JsonBytesMappingKind::kHexId`  | Encoding used for trace_id and span_id                            |
|`use_json_name`     | n/a                                | `false`                         | Whether to use json name of protobuf field to set the key of json |
|`timeout`           |`OTEL_EXPORTER_OTLP_TIMEOUT`        | `10s`                           | http timeout                                                      |
|                    |`OTEL_EXPORTER_OTLP_TRACES_TIMEOUT` |                                 |                                                                   |
|`http_headers`      |`OTEL_EXPORTER_OTLP_HEADERS`        |                                 | http headers                                                      |
|                    |`OTEL_EXPORTER_OTLP_TRACES_HEADERS` |                                 |                                                                   |

### Configuration options ( OTLP File Exporter )

| Option             | Env Variable                       | Default                         | Description                                                       |
|--------------------|------------------------------------|---------------------------------|-------------------------------------------------------------------|
|`backend_options`   | n/a                                |`OtlpFileClientFileSystemOptions`| The OTLP FILE backend                                             |

The backend for OTLP File Exporter can be `OtlpFileClientFileSystemOptions` (which
support basic log rotate and alias), and reference to `std::ostream` or custom `OtlpFileAppender`.

#### Configuration options ( File System Backend for OTLP File Exporter )

| Option             | Env Variable                       | Default                         | Description                                                       |
|--------------------|------------------------------------|---------------------------------|-------------------------------------------------------------------|
|`file_pattern`      | n/a                                |`trace-%N.jsonl`                 | The file pattern to use                                           |
|                    |                                    |`metrics-%N.jsonl`               |                                                                   |
|                    |                                    |`logs-%N.jsonl`                  |                                                                   |
|`alias_pattern`     | n/a                                | `trace-latest.jsonl`            | The file which always point to the latest file                    |
|                    |                                    | `metrics-latest.jsonl`          |                                                                   |
|                    |                                    | `logs-latest.jsonl`             |                                                                   |
|`flush_interval`    | n/a                                | `30s`                           | Interval to force flush ostream                                   |
|`flush_count`       | n/a                                | `256`                           | Force flush ostream every `flush_count` records                   |
|`file_size`         | n/a                                | `20MB`                          | File size to rotate log files                                     |
|`rotate_size`       | n/a                                |                                 | Rotate count                                                      |

Some special placeholders are available for `file_pattern` and `alias_pattern`:

+ `%Y`:  Writes year as a 4 digit decimal number
+ `%y`:  Writes last 2 digits of year as a decimal number (range [00,99])
+ `%m`:  Writes month as a decimal number (range [01,12])
+ `%j`:  Writes day of the year as a decimal number (range [001,366])
+ `%d`:  Writes day of the month as a decimal number (range [01,31])
+ `%w`:  Writes weekday as a decimal number, where Sunday is 0 (range [0-6])
+ `%H`:  Writes hour as a decimal number, 24 hour clock (range [00-23])
+ `%I`:  Writes hour as a decimal number, 12 hour clock (range [01,12])
+ `%M`:  Writes minute as a decimal number (range [00,59])
+ `%S`:  Writes second as a decimal number (range [00,60])
+ `%F`:  Equivalent to "%Y-%m-%d" (the ISO 8601 date format)
+ `%T`:  Equivalent to "%H:%M:%S" (the ISO 8601 time format)
+ `%R`:  Equivalent to "%H:%M"
+ `%N`:  Rotate index, start from 0
+ `%n`:  Rotate index, start from 1

## Example

For a complete example demonstrating how to use the OTLP exporter, see
[examples/otlp](https://github.com/open-telemetry/opentelemetry-cpp/blob/main/examples/otlp/).
