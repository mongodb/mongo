// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knob_descriptors_integration.h"
#include "mongo/db/query/query_knobs/query_knob_change_notifier.h"
#include "mongo/db/query/query_stats/query_stats_on_parameter_change.h"

namespace mongo {

/**
 * Keeps the queryStats rate limiter in sync with a Parameterized Query Settings override of the
 * 'queryStatsSampleRate' query knob. Lives in the query_stats module since it forwards to
 * query-stats-specific logic; the query_knobs module stays free of knob-specific behavior.
 */
REGISTER_QUERY_KNOBS_LISTENER(QueryStatsSamplingRateLimiterUpdater,
                              ([](const QueryKnobChange& change) {
                                  if (change.id != query_knobs::kQueryStatsSampleRate.id) {
                                      return Status::OK();
                                  }
                                  return query_stats_util::onQueryStatsSamplingRateUpdate(
                                      std::get<double>(change.newValue));
                              }))

}  // namespace mongo
