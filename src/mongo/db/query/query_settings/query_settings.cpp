// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_settings.h"

#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/query_knobs/query_knob_snapshot.h"
#include "mongo/db/query/query_settings/query_settings_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::query_settings {

namespace {
const QuerySettings kEmpty{};
}  // namespace

const QuerySettings& forOp(OperationContext* opCtx) {
    using namespace query_settings_details;
    return std::visit(
        OverloadedVisitor{
            // No eligible command has begun: no settings apply.
            [&](const NotStarted&) -> const QuerySettings& { return kEmpty; },
            // Eligible but not yet resolved: reading now would observe stale values.
            [&](const Pending&) -> const QuerySettings& {
                // A nested DBDirectClient command may run before the user command resolves its
                // settings (e.g. loading 'system.js' while parsing a $where filter). From the
                // nested command's perspective the settings were never initiated, so it observes
                // none.
                if (opCtx->getClient()->isInDirectClient()) {
                    return kEmpty;
                }
                tasserted(13020703, "query settings read while resolution is still pending");
            },
            // Resolution matched nothing: no settings apply.
            [&](const Empty&) -> const QuerySettings& { return kEmpty; },
            [&](const QuerySettings& settings) -> const QuerySettings& { return settings; },
        },
        getQuerySettingsStateForOp(opCtx));
}

KnobOverrideResult tryOverrideQueryKnobValues(OperationContext* opCtx,
                                              QueryKnobSnapshot& snapshot) {
    using namespace query_settings_details;
    return std::visit(OverloadedVisitor{
                          // No eligible command has begun yet; the caller must retry later.
                          [&](const NotStarted&) { return KnobOverrideResult::kNotStarted; },
                          // Settings are still pending resolution; the caller must retry later.
                          [&](const Pending&) {
                              // A nested DBDirectClient command running inside the user command's
                              // pending window observes no settings (see 'forOp'): report
                              // 'kNotStarted' so its knob reads are unrestricted and the outer
                              // command still installs its overrides on a later retry.
                              if (opCtx->getClient()->isInDirectClient()) {
                                  return KnobOverrideResult::kNotStarted;
                              }
                              return KnobOverrideResult::kPending;
                          },
                          // Not eligible, or resolution matched nothing: no overrides to apply.
                          [&](const Empty&) { return KnobOverrideResult::kApplied; },
                          [&](const QuerySettings& settings) {
                              const bool hasOverrides =
                                  settings.getQueryKnobs() || settings.getQueryFramework();
                              if (!hasOverrides) {
                                  // Nothing to override.
                                  return KnobOverrideResult::kApplied;
                              }
                              QueryKnobSnapshotBuilder builder(std::move(snapshot));
                              if (auto&& knobs = settings.getQueryKnobs()) {
                                  for (auto&& [id, value] : knobs->entries()) {
                                      builder.set(id, value, KnobSource::kQuerySettings);
                                  }
                              }
                              // TODO SERVER-129207 Migrate query framework control from
                              // QuerySettings to QueryKnobs and remove this special case.
                              if (auto&& framework = settings.getQueryFramework()) {
                                  builder.set(query_knobs::kQueryFrameworkControl.id,
                                              QueryKnobValue(static_cast<int>(*framework)),
                                              KnobSource::kQuerySettings);
                              }
                              snapshot = std::move(builder).build();
                              return KnobOverrideResult::kApplied;
                          },
                      },
                      getQuerySettingsStateForOp(opCtx));
}

void addQuerySettingsToSlowLog(OperationContext* opCtx, logv2::DynamicAttributes& attrs) {
    auto& state = query_settings_details::getQuerySettingsStateForOp(opCtx);
    if (auto* settings = std::get_if<QuerySettings>(&state)) {
        attrs.add("querySettings", settings->toBSON());
    }
}

}  // namespace mongo::query_settings
