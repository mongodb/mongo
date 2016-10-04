/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/db/ftdc/block_compressor.h"
#include "mongo/db/jsobj.h"

namespace mongo {

/**
 * Inflates a compressed chunk of metrics into a list of BSON documents
 */
class FTDCDecompressor {
    MONGO_DISALLOW_COPYING(FTDCDecompressor);

public:
    FTDCDecompressor() = default;

    /**
     * Inflates a compressed chunk of metrics into a vector of owned BSON documents.
     *
     * Will fail if the chunk is corrupt or too short.
     *
     * Returns N samples where N = sample count + 1. The 1 is the reference document.
     */
    StatusWith<std::vector<BSONObj>> uncompress(ConstDataRange buf);

private:
    BlockCompressor _compressor;
};

}  // namespace mongo
