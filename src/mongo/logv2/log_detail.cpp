/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/logv2/attributes.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_internal.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/log_source.h"

namespace mongo::logv2::detail {

void doLogImpl(int32_t id,
               LogSeverity const& severity,
               LogOptions const& options,
               StringData message,
               TypeErasedAttributeStorage const& attrs) {
    dassert(options.component() != LogComponent::kNumLogComponents);
    auto& source = options.domain().internal().source();
    auto record = source.open_record(id,
                                     severity,
                                     options.component(),
                                     options.tags(),
                                     options.truncation(),
                                     options.uassertErrorCode());
    if (record) {
        record.attribute_values().insert(
            attributes::message(),
            boost::log::attribute_value(
                new boost::log::attributes::attribute_value_impl<StringData>(message)));

        record.attribute_values().insert(
            attributes::attributes(),
            boost::log::attribute_value(
                new boost::log::attributes::attribute_value_impl<TypeErasedAttributeStorage>(
                    attrs)));

        source.push_record(std::move(record));
    }
}

}  // namespace mongo::logv2::detail
