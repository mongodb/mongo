/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_attributes-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace log
{

/**
  The basename of the file.
 */
static constexpr const char *kLogFileName = "log.file.name";

/**
  The basename of the file, with symlinks resolved.
 */
static constexpr const char *kLogFileNameResolved = "log.file.name_resolved";

/**
  The full path to the file.
 */
static constexpr const char *kLogFilePath = "log.file.path";

/**
  The full path to the file, with symlinks resolved.
 */
static constexpr const char *kLogFilePathResolved = "log.file.path_resolved";

/**
  The stream associated with the log. See below for a list of well-known values.
 */
static constexpr const char *kLogIostream = "log.iostream";

/**
  The complete original Log Record.
  <p>
  This value MAY be added when processing a Log Record which was originally transmitted as a string
  or equivalent data type AND the Body field of the Log Record does not contain the same value.
  (e.g. a syslog or a log record read from a file.)
 */
static constexpr const char *kLogRecordOriginal = "log.record.original";

/**
  A unique identifier for the Log Record.
  <p>
  If an id is provided, other log records with the same id will be considered duplicates and can be
  removed safely. This means, that two distinguishable log records MUST have different values. The
  id MAY be an <a href="https://github.com/ulid/spec">Universally Unique Lexicographically Sortable
  Identifier (ULID)</a>, but other identifiers (e.g. UUID) may be used as needed.
 */
static constexpr const char *kLogRecordUid = "log.record.uid";

namespace LogIostreamValues
{
/**
  Logs from stdout stream
 */
static constexpr const char *kStdout = "stdout";

/**
  Events from stderr stream
 */
static constexpr const char *kStderr = "stderr";

}  // namespace LogIostreamValues

}  // namespace log
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
