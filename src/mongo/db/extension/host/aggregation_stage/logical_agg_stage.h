// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo::extension::host {

/**
 * Host-defined LogicalAggStage node.
 *
 * Wraps a DocumentSource such that a host-defined logical stage can forward information about the
 * document source to the extension stage.
 */
class LogicalAggStage {
public:
    ~LogicalAggStage() = default;

    std::string_view getName() const {
        return _stageName;
    }

    BSONObj getFilter() const {
        return _documentSource->hasQuery() ? _documentSource->getQuery() : BSONObj();
    }

    static inline std::unique_ptr<LogicalAggStage> make(DocumentSource* docSrc) {
        return std::unique_ptr<LogicalAggStage>(new LogicalAggStage(docSrc));
    }

protected:
    LogicalAggStage(absl::Nonnull<DocumentSource*> documentSource)
        : _documentSource(documentSource), _stageName(documentSource->getSourceName()) {}

private:
    DocumentSource* const _documentSource;
    const std::string _stageName;
};

};  // namespace mongo::extension::host
