// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/external_data_source_option_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

namespace mongo {
using namespace std::literals::string_view_literals;
/**
 * Metadata for external data source.
 */
struct [[MONGO_MOD_NEEDS_REPLACEMENT]] ExternalDataSourceMetadata {
    static constexpr auto kUrlProtocolFile = "file://"sv;

    ExternalDataSourceMetadata(std::string_view urlStr,
                               StorageTypeEnum storageTypeEnum,
                               FileTypeEnum fileTypeEnum)
        : url(urlStr), storageType(storageTypeEnum), fileType(fileTypeEnum) {
        uassert(6968500,
                fmt::format("File url must start with {}", kUrlProtocolFile),
                urlStr.starts_with(kUrlProtocolFile));
        uassert(6968501, "Storage type must be 'pipe'", storageType == StorageTypeEnum::pipe);
        uassert(6968502, "File type must be 'bson'", fileType == FileTypeEnum::bson);
    }

    ExternalDataSourceMetadata(const ExternalDataSourceInfo& dataSourceInfo)
        : ExternalDataSourceMetadata(dataSourceInfo.getUrl(),
                                     dataSourceInfo.getStorageType(),
                                     dataSourceInfo.getFileType()) {}

    /**
     * Url for an external data source
     */
    std::string url;

    /**
     * Storage type for an external data source
     */
    StorageTypeEnum storageType;

    /**
     * File type for an external data source
     */
    FileTypeEnum fileType;
};

/**
 * Options for virtual collection.
 */
struct [[MONGO_MOD_NEEDS_REPLACEMENT]] VirtualCollectionOptions {
    VirtualCollectionOptions() : dataSources() {}
    VirtualCollectionOptions(std::vector<ExternalDataSourceMetadata> dataSources)
        : dataSources(std::move(dataSources)) {}
    VirtualCollectionOptions(const std::vector<ExternalDataSourceInfo>& options) {
        for (auto&& dataSource : options) {
            dataSources.emplace_back(dataSource);
        }
    }

    /**
     * Specification for external data sources.
     */
    std::vector<ExternalDataSourceMetadata> dataSources;
};
}  // namespace mongo
