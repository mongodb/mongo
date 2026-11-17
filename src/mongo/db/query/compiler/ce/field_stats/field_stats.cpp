// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/field_stats/field_stats.h"

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>

namespace mongo::ce {
namespace {

// Backslash-escapes '|' (the _id separator) and '\' itself, so that field paths containing
// either character cannot collide with another path list in the _id string.
std::string escapeFieldPath(const std::string& path) {
    std::string escaped;
    escaped.reserve(path.size());
    for (const char c : path) {
        if (c == '\\' || c == '|') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}

}  // namespace

std::string makeFieldStatsId(const UUID& collectionUuid, std::vector<std::string> fieldPaths) {
    tassert(13175805, "A field-stats id requires at least one field path", !fieldPaths.empty());
    std::sort(fieldPaths.begin(), fieldPaths.end());

    str::stream id;
    id << kFieldStatsSchemaVersion << "|" << collectionUuid.toString();
    for (const auto& path : fieldPaths) {
        id << "|" << escapeFieldPath(path);
    }
    return id;
}

void validateFieldStatsPath(std::string_view path) {
    const FieldRef fieldRef(path);
    uassert(6799703, "Key path is empty", !fieldRef.empty());

    const auto numericPathComponents = fieldRef.getNumericPathComponents(0);
    uassert(6799704,
            str::stream() << "Key path contains numeric component "
                          << fieldRef.getPart(*(numericPathComponents.begin())),
            numericPathComponents.empty());

    for (size_t i = 0; i < fieldRef.numParts(); ++i) {
        uassertStatusOK(FieldPath::validateFieldName(fieldRef.getPart(i)));
    }
}

Status SingleFieldStats::validate() const {
    if (ndv < 0.0) {
        return {ErrorCodes::BadValue, str::stream() << "ndv (" << ndv << ") must not be negative"};
    }
    const double countedDocs = static_cast<double>(nonScalarDocsNum) +
        static_cast<double>(nullDocsNum) + static_cast<double>(missingDocsNum) +
        static_cast<double>(undefinedDocsNum) + static_cast<double>(headDocsNum);
    if (countedDocs > static_cast<double>(totalDocsNum)) {
        return {ErrorCodes::BadValue,
                str::stream() << "nonScalarDocsNum + nullDocsNum + missingDocsNum + "
                                 "undefinedDocsNum + headDocsNum ("
                              << countedDocs << ") must not exceed totalDocsNum (" << totalDocsNum
                              << ")"};
    }
    return Status::OK();
}

double SingleFieldStats::residualDocsNum() const {
    return std::max(0.0,
                    static_cast<double>(totalDocsNum) -
                        (static_cast<double>(nonScalarDocsNum) + static_cast<double>(nullDocsNum) +
                         static_cast<double>(missingDocsNum) +
                         static_cast<double>(undefinedDocsNum) + static_cast<double>(headDocsNum)));
}

double SingleFieldStats::residualNdv() const {
    return std::max(0.0,
                    ndv - static_cast<double>(mcv.size()) - (nullDocsNum > 0 ? 1.0 : 0.0) -
                        (missingDocsNum > 0 || undefinedDocsNum > 0 ? 1.0 : 0.0));
}

Status CompoundFieldStats::validate() const {
    if (ndvs.size() < 4 || ndvs.size() > kMaxFieldStatsFields + 2) {
        return {ErrorCodes::BadValue,
                str::stream() << "Compound field statistics need n + 2 NDVs for 2 <= n <= "
                              << kMaxFieldStatsFields << ", found " << ndvs.size()};
    }
    for (size_t i = 0; i < ndvs.size(); ++i) {
        if (ndvs[i] < 0.0) {
            return {ErrorCodes::BadValue,
                    str::stream() << "ndv[" << i << "] (" << ndvs[i] << ") must not be negative"};
        }
    }
    return Status::OK();
}

void CompoundFieldStats::capNdvs() {
    for (auto& ndv : ndvs) {
        ndv = std::min(ndv, static_cast<double>(totalDocsNum));
    }
}

StatusWith<SingleFieldStats> makeSingleFieldStatsFromDoc(const FieldStatsDoc& doc) {
    const auto& ndvSketches = doc.getNdv().getSketches();
    const auto& mcvSketches = doc.getMcv().getSketches();
    if (ndvSketches.size() != 1 || mcvSketches.size() != 1) {
        return {ErrorCodes::BadValue,
                str::stream() << "Single-field statistics need one NDV and one MCV sketch, found "
                              << ndvSketches.size() << " and " << mcvSketches.size()};
    }

    SingleFieldStats stats{.ndv = static_cast<double>(ndvSketches.front().getNdv()),
                           .nonScalarDocsNum = static_cast<size_t>(doc.getNonScalarDocs()),
                           .nullDocsNum = static_cast<size_t>(doc.getNullDocs()),
                           .missingDocsNum = static_cast<size_t>(doc.getMissingDocs()),
                           .undefinedDocsNum = static_cast<size_t>(doc.getUndefinedDocs()),
                           .totalDocsNum = static_cast<size_t>(doc.getTotalDocs())};

    const auto& entries = mcvSketches.front().getEntries();
    stats.mcv.reserve(entries.size());
    for (const auto& entry : entries) {
        const auto estFreq = static_cast<size_t>(entry.getEstFreq());
        const auto err = static_cast<size_t>(entry.getErr());
        if (err > estFreq) {
            return {ErrorCodes::BadValue, "An MCV entry's err must not exceed its estFreq"};
        }
        auto value = sbe::bson::convertToOwned(entry.getValue().getElement());
        const bool inserted =
            stats.mcv.emplace(stats::SBEValue(value.releaseToRaw()), McvEntryStats{estFreq, err})
                .second;
        if (!inserted) {
            return {ErrorCodes::BadValue, "The MCV list must not contain duplicate values"};
        }
        stats.headDocsNum += estFreq - err;
    }

    // 'ndv' is an HLL estimate and may exceed the number of documents.
    stats.ndv = std::min(stats.ndv, static_cast<double>(stats.totalDocsNum));
    if (auto status = stats.validate(); !status.isOK()) {
        return status;
    }
    return stats;
}

StatusWith<CompoundFieldStats> makeCompoundFieldStatsFromDoc(const FieldStatsDoc& doc,
                                                             size_t numFields) {
    if (numFields < 2 || numFields > kMaxFieldStatsFields) {
        return {ErrorCodes::BadValue,
                str::stream() << "Compound field statistics cover 2 to " << kMaxFieldStatsFields
                              << " fields, requested " << numFields};
    }
    const auto& ndvSketches = doc.getNdv().getSketches();
    if (ndvSketches.size() != numFields + 2) {
        return {ErrorCodes::BadValue,
                str::stream() << "Statistics over " << numFields << " fields need " << numFields + 2
                              << " NDV sketches, found " << ndvSketches.size()};
    }

    CompoundFieldStats stats{.totalDocsNum = static_cast<size_t>(doc.getTotalDocs())};
    for (const auto& sketch : ndvSketches) {
        stats.ndvs.push_back(static_cast<double>(sketch.getNdv()));
    }
    stats.capNdvs();
    if (auto status = stats.validate(); !status.isOK()) {
        return status;
    }
    return stats;
}

}  // namespace mongo::ce
