/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/log/core/record_view.hpp>
#include <boost/log/utility/formatting_ostream_fwd.hpp>

#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_format.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/log_truncation.h"
#include "mongo/util/time_support.h"

namespace mongo::logv2 {

class JSONFormatter {
public:
    JSONFormatter(const AtomicWord<int32_t>* maxAttributeSizeKB = nullptr,
                  LogTimestampFormat timestampFormat = LogTimestampFormat::kISO8601UTC)
        : _maxAttributeSizeKB(maxAttributeSizeKB), _timestampFormat(timestampFormat) {}

    void format(fmt::memory_buffer& buffer,
                LogSeverity severity,
                LogComponent component,
                Date_t date,
                int32_t id,
                StringData context,
                StringData message,
                const TypeErasedAttributeStorage& attrs,
                LogTag tags,
                LogTruncation truncation) const;
    void operator()(boost::log::record_view const& rec, boost::log::formatting_ostream& strm) const;

private:
    const AtomicWord<int32_t>* _maxAttributeSizeKB;
    const LogTimestampFormat _timestampFormat;
};

}  // namespace mongo::logv2
