/*    Copyright 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <cstring>

#include "mongo/base/data_type.h"

namespace mongo {

template <>
struct DataType::Handler<StringData> {
    static Status load(StringData* sdata,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset) {
        if (sdata) {
            *sdata = StringData(ptr, length);
        }

        if (advanced) {
            *advanced = length;
        }

        return Status::OK();
    }

    static Status store(const StringData& sdata,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debug_offset) {
        if (sdata.size() > length) {
            return makeStoreStatus(sdata, length, debug_offset);
        }

        if (ptr) {
            std::memcpy(ptr, sdata.rawData(), sdata.size());
        }

        if (advanced) {
            *advanced = sdata.size();
        }

        return Status::OK();
    }

    static StringData defaultConstruct() {
        return StringData();
    }

private:
    static Status makeStoreStatus(const StringData& sdata,
                                  size_t length,
                                  std::ptrdiff_t debug_offset);
};

}  // namespace mongo
