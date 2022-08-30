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

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/logv2/log_util.h"
#include "mongo/logv2/logv2_options_gen.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

void RedactEncryptedFields::append(OperationContext* opCtx,
                                   BSONObjBuilder* b,
                                   StringData name,
                                   const boost::optional<TenantId>&) {
    *b << name << logv2::shouldRedactBinDataEncrypt();
}

Status RedactEncryptedFields::set(const BSONElement& newValueElement,
                                  const boost::optional<TenantId>&) {
    bool newVal;
    if (!newValueElement.coerce(&newVal)) {
        return {ErrorCodes::BadValue,
                str::stream() << "Invalid value for redactEncryptedFields: " << newValueElement};
    }

    logv2::setShouldRedactBinDataEncrypt(newVal);
    return Status::OK();
}

Status RedactEncryptedFields::setFromString(StringData str, const boost::optional<TenantId>&) {
    if (str == "true" || str == "1") {
        logv2::setShouldRedactBinDataEncrypt(true);
    } else if (str == "false" || str == "0") {
        logv2::setShouldRedactBinDataEncrypt(false);
    } else {
        return {ErrorCodes::BadValue,
                str::stream() << "Invalid value for redactEncryptedFields: " << str};
    }
    return Status::OK();
}

}  // namespace mongo
