// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/persistence/persistent_stats_config_update.h"

#include "mongo/db/query/compiler/ce/persistence/persistent_stats_config_gen.h"

#include <cstdint>

namespace mongo {

namespace {
boost::optional<std::int32_t> toStoredSampleSize(const SampleSizeUpdate& sampleSize) {
    if (const auto* setSampleSize = std::get_if<SetSampleSize>(&sampleSize)) {
        return setSampleSize->value;
    }
    return boost::none;
}
}  // namespace

boost::optional<PersistentStatsConfig> applyPersistentStatsConfigUpdate(
    const boost::optional<PersistentStatsConfig>& current,
    const PersistentStatsConfigUpdate& update) {
    boost::optional<std::int32_t> sampleSize;
    boost::optional<std::int32_t> joinSampleSize;
    if (current && current->getSamples()) {
        sampleSize = current->getSamples()->getSampleSize();
        joinSampleSize = current->getSamples()->getJoinSampleSize();
    }

    if (const auto& samples = update.getSamples()) {
        if (const auto& updatedSampleSize = samples->getSampleSize()) {
            sampleSize = toStoredSampleSize(*updatedSampleSize);
        }
        if (const auto& updatedJoinSampleSize = samples->getJoinSampleSize()) {
            joinSampleSize = toStoredSampleSize(*updatedJoinSampleSize);
        }
    }

    if (!sampleSize && !joinSampleSize) {
        return boost::none;
    }

    PersistentStatsSamplesConfig samples;
    samples.setSampleSize(sampleSize);
    samples.setJoinSampleSize(joinSampleSize);

    PersistentStatsConfig config;
    config.setSamples(samples);
    return config;
}

}  // namespace mongo
