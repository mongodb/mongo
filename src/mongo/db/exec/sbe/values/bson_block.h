// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/path_request.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::sbe::value {
/**
 * Interface for extracting CellBlocks from raw BSON. Use makeExtractor(), below, to create an
 * instance. The implementation details are hidden in the associated .cpp file.
 */
class BSONCellExtractor {
public:
    /*
     * Creates a BSON extractor which will collect the given path requests.
     */
    static std::unique_ptr<BSONCellExtractor> make(const std::vector<PathRequest>& pathReqs);


    virtual ~BSONCellExtractor() = default;

    /**
     * Given a bunch of BSON objects, extract a set of paths from them into CellBlocks.
     */
    virtual std::vector<std::unique_ptr<CellBlock>> extractFromBsons(
        const std::vector<BSONObj>& bsons) = 0;

    /**
     * Given a bunch of top-level fields (as a tag, val pair), extract the set of sub-paths from
     * them into CellBlocks. This is useful when, for example, we have the value for field 'a'
     * sitting in memory, materialized, and we want to avoid wrapping 'a' in a parent BSON object.
     */
    virtual std::vector<std::unique_ptr<CellBlock>> extractFromTopLevelField(
        std::string_view topLevelField,
        const std::span<const TypeTags>& tags,
        const std::span<const Value>& vals) = 0;
};

/**
 * Given a vector of PathRequests and BSON objects, produces one CellBlock
 * per path request, with data from the BSON Obj.
 *
 * All returned data is fully owned by the CellBlocks.
 */
std::vector<std::unique_ptr<CellBlock>> extractCellBlocksFromBsons(
    const std::vector<PathRequest>& pathReqs, const std::vector<BSONObj>& bsons);

/**
 * Given a BSONObj and PathRequest, return a vector of the value pointers requested by the path.
 */
std::vector<const char*> extractValuePointersFromBson(BSONObj& obj,
                                                      PathRequest pathReqs,
                                                      bool traverseArrays);
}  // namespace mongo::sbe::value
