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
namespace cassandra
{

/**
  The consistency level of the query. Based on consistency values from <a
  href="https://docs.datastax.com/en/cassandra-oss/3.0/cassandra/dml/dmlConfigConsistency.html">CQL</a>.
 */
static constexpr const char *kCassandraConsistencyLevel = "cassandra.consistency.level";

/**
  The data center of the coordinating node for a query.
 */
static constexpr const char *kCassandraCoordinatorDc = "cassandra.coordinator.dc";

/**
  The ID of the coordinating node for a query.
 */
static constexpr const char *kCassandraCoordinatorId = "cassandra.coordinator.id";

/**
  The fetch size used for paging, i.e. how many rows will be returned at once.
 */
static constexpr const char *kCassandraPageSize = "cassandra.page.size";

/**
  Whether or not the query is idempotent.
 */
static constexpr const char *kCassandraQueryIdempotent = "cassandra.query.idempotent";

/**
  The number of times a query was speculatively executed. Not set or @code 0 @endcode if the query
  was not executed speculatively.
 */
static constexpr const char *kCassandraSpeculativeExecutionCount =
    "cassandra.speculative_execution.count";

namespace CassandraConsistencyLevelValues
{
/**
  All
 */
static constexpr const char *kAll = "all";

/**
  Each Quorum
 */
static constexpr const char *kEachQuorum = "each_quorum";

/**
  Quorum
 */
static constexpr const char *kQuorum = "quorum";

/**
  Local Quorum
 */
static constexpr const char *kLocalQuorum = "local_quorum";

/**
  One
 */
static constexpr const char *kOne = "one";

/**
  Two
 */
static constexpr const char *kTwo = "two";

/**
  Three
 */
static constexpr const char *kThree = "three";

/**
  Local One
 */
static constexpr const char *kLocalOne = "local_one";

/**
  Any
 */
static constexpr const char *kAny = "any";

/**
  Serial
 */
static constexpr const char *kSerial = "serial";

/**
  Local Serial
 */
static constexpr const char *kLocalSerial = "local_serial";

}  // namespace CassandraConsistencyLevelValues

}  // namespace cassandra
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
