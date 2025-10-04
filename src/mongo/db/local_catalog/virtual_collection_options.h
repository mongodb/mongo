/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/external_data_source_option_gen.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <vector>

namespace mongo {
/**
 * Metadata for external data source.
 */
struct ExternalDataSourceMetadata {
    static constexpr auto kUrlProtocolFile = "file://"_sd;

    ExternalDataSourceMetadata(StringData urlStr,
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
struct VirtualCollectionOptions {
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
