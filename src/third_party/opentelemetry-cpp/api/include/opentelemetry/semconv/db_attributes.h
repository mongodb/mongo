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
namespace db
{

/**
  The name of a collection (table, container) within the database.
  <p>
  It is RECOMMENDED to capture the value as provided by the application
  without attempting to do any case normalization.
  <p>
  The collection name SHOULD NOT be extracted from @code db.query.text @endcode,
  when the database system supports query text with multiple collections
  in non-batch operations.
  <p>
  For batch operations, if the individual operations are known to have the same
  collection name then that collection name SHOULD be used.
 */
static constexpr const char *kDbCollectionName = "db.collection.name";

/**
  The name of the database, fully qualified within the server address and port.
  <p>
  If a database system has multiple namespace components, they SHOULD be concatenated from the most
  general to the most specific namespace component, using @code | @endcode as a separator between
  the components. Any missing components (and their associated separators) SHOULD be omitted.
  Semantic conventions for individual database systems SHOULD document what @code db.namespace
  @endcode means in the context of that system. It is RECOMMENDED to capture the value as provided
  by the application without attempting to do any case normalization.
 */
static constexpr const char *kDbNamespace = "db.namespace";

/**
  The number of queries included in a batch operation.
  <p>
  Operations are only considered batches when they contain two or more operations, and so @code
  db.operation.batch.size @endcode SHOULD never be @code 1 @endcode.
 */
static constexpr const char *kDbOperationBatchSize = "db.operation.batch.size";

/**
  The name of the operation or command being executed.
  <p>
  It is RECOMMENDED to capture the value as provided by the application
  without attempting to do any case normalization.
  <p>
  The operation name SHOULD NOT be extracted from @code db.query.text @endcode,
  when the database system supports query text with multiple operations
  in non-batch operations.
  <p>
  If spaces can occur in the operation name, multiple consecutive spaces
  SHOULD be normalized to a single space.
  <p>
  For batch operations, if the individual operations are known to have the same operation name
  then that operation name SHOULD be used prepended by @code BATCH  @endcode,
  otherwise @code db.operation.name @endcode SHOULD be @code BATCH @endcode or some other database
  system specific term if more applicable.
 */
static constexpr const char *kDbOperationName = "db.operation.name";

/**
  Low cardinality summary of a database query.
  <p>
  The query summary describes a class of database queries and is useful
  as a grouping key, especially when analyzing telemetry for database
  calls involving complex queries.
  <p>
  Summary may be available to the instrumentation through
  instrumentation hooks or other means. If it is not available, instrumentations
  that support query parsing SHOULD generate a summary following
  <a href="/docs/database/database-spans.md#generating-a-summary-of-the-query">Generating query
  summary</a> section.
 */
static constexpr const char *kDbQuerySummary = "db.query.summary";

/**
  The database query being executed.
  <p>
  For sanitization see <a
  href="/docs/database/database-spans.md#sanitization-of-dbquerytext">Sanitization of @code
  db.query.text @endcode</a>. For batch operations, if the individual operations are known to have
  the same query text then that query text SHOULD be used, otherwise all of the individual query
  texts SHOULD be concatenated with separator @code ;  @endcode or some other database system
  specific separator if more applicable. Parameterized query text SHOULD NOT be sanitized. Even
  though parameterized query text can potentially have sensitive data, by using a parameterized
  query the user is giving a strong signal that any sensitive data will be passed as parameter
  values, and the benefit to observability of capturing the static part of the query text by default
  outweighs the risk.
 */
static constexpr const char *kDbQueryText = "db.query.text";

/**
  Database response status code.
  <p>
  The status code returned by the database. Usually it represents an error code, but may also
  represent partial success, warning, or differentiate between various types of successful outcomes.
  Semantic conventions for individual database systems SHOULD document what @code
  db.response.status_code @endcode means in the context of that system.
 */
static constexpr const char *kDbResponseStatusCode = "db.response.status_code";

/**
  The name of a stored procedure within the database.
  <p>
  It is RECOMMENDED to capture the value as provided by the application
  without attempting to do any case normalization.
  <p>
  For batch operations, if the individual operations are known to have the same
  stored procedure name then that stored procedure name SHOULD be used.
 */
static constexpr const char *kDbStoredProcedureName = "db.stored_procedure.name";

/**
  The database management system (DBMS) product as identified by the client instrumentation.
  <p>
  The actual DBMS may differ from the one identified by the client. For example, when using
  PostgreSQL client libraries to connect to a CockroachDB, the @code db.system.name @endcode is set
  to @code postgresql @endcode based on the instrumentation's best knowledge.
 */
static constexpr const char *kDbSystemName = "db.system.name";

namespace DbSystemNameValues
{
/**
  <a href="https://mariadb.org/">MariaDB</a>
 */
static constexpr const char *kMariadb = "mariadb";

/**
  <a href="https://www.microsoft.com/sql-server">Microsoft SQL Server</a>
 */
static constexpr const char *kMicrosoftSqlServer = "microsoft.sql_server";

/**
  <a href="https://www.mysql.com/">MySQL</a>
 */
static constexpr const char *kMysql = "mysql";

/**
  <a href="https://www.postgresql.org/">PostgreSQL</a>
 */
static constexpr const char *kPostgresql = "postgresql";

}  // namespace DbSystemNameValues

}  // namespace db
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
