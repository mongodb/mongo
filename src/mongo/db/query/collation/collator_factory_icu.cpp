/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/collation/collator_factory_icu.h"

#include <unicode/errorcode.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/collation/collator_interface_icu.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

// Extracts the collation options from 'spec' and performs basic validation.
//
// Validation or normalization requiring the ICU library is done later.
StatusWith<CollationSpec> parseToCollationSpec(const BSONObj& spec) {
    CollationSpec parsedSpec;

    for (auto elem : spec) {
        if (str::equals(CollationSpec::kLocaleField, elem.fieldName())) {
            if (elem.type() != BSONType::String) {
                return {ErrorCodes::FailedToParse,
                        str::stream() << "Field '" << CollationSpec::kLocaleField
                                      << "' must be of type string in: " << spec};
            }

            parsedSpec.localeID = elem.String();
        } else {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "Unknown collation spec field: " << elem.fieldName()};
        }
    }

    if (parsedSpec.localeID.empty()) {
        return {ErrorCodes::FailedToParse, str::stream() << "Missing locale string"};
    }

    return parsedSpec;
}

}  // namespace

StatusWith<std::unique_ptr<CollatorInterface>> CollatorFactoryICU::makeFromBSON(
    const BSONObj& spec) {
    auto parsedSpec = parseToCollationSpec(spec);
    if (!parsedSpec.isOK()) {
        return parsedSpec.getStatus();
    }

    // TODO SERVER-22373: use ICU to validate and normalize locale string. As part of this work, we
    // should add unit test coverage for both locale string validation and normalization.
    auto locale = icu::Locale::createFromName(parsedSpec.getValue().localeID.c_str());

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Collator> icuCollator(icu::Collator::createInstance(locale, status));
    if (U_FAILURE(status)) {
        icu::ErrorCode icuError;
        icuError.set(status);
        return {ErrorCodes::OperationFailed,
                str::stream() << "Failed to create collator: " << icuError.errorName()
                              << ". Collation spec: " << spec};
    }

    auto mongoCollator = stdx::make_unique<CollatorInterfaceICU>(std::move(parsedSpec.getValue()),
                                                                 std::move(icuCollator));
    return {std::move(mongoCollator)};
}

}  // namespace mongo
