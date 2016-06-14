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

#include "mongo/db/query/collation/collation_spec.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

namespace mongo {

const char* CollationSpec::kLocaleField = "locale";
const char* CollationSpec::kCaseLevelField = "caseLevel";
const char* CollationSpec::kCaseFirstField = "caseFirst";
const char* CollationSpec::kStrengthField = "strength";
const char* CollationSpec::kNumericOrderingField = "numericOrdering";
const char* CollationSpec::kAlternateField = "alternate";
const char* CollationSpec::kMaxVariableField = "maxVariable";
const char* CollationSpec::kNormalizationField = "normalization";
const char* CollationSpec::kBackwardsField = "backwards";
const char* CollationSpec::kVersionField = "version";
const char* CollationSpec::kSimpleBinaryComparison = "simple";
const char* CollationSpec::kCaseFirstUpper = "upper";
const char* CollationSpec::kCaseFirstLower = "lower";
const char* CollationSpec::kCaseFirstOff = "off";
const char* CollationSpec::kAlternateNonIgnorable = "non-ignorable";
const char* CollationSpec::kAlternateShifted = "shifted";
const char* CollationSpec::kMaxVariablePunct = "punct";
const char* CollationSpec::kMaxVariableSpace = "space";

BSONObj CollationSpec::toBSON() const {
    BSONObjBuilder builder;
    builder.append(CollationSpec::kLocaleField, localeID);
    builder.append(CollationSpec::kCaseLevelField, caseLevel);

    switch (caseFirst) {
        case CollationSpec::CaseFirstType::kUpper:
            builder.append(CollationSpec::kCaseFirstField, CollationSpec::kCaseFirstUpper);
            break;
        case CollationSpec::CaseFirstType::kLower:
            builder.append(CollationSpec::kCaseFirstField, CollationSpec::kCaseFirstLower);
            break;
        case CollationSpec::CaseFirstType::kOff:
            builder.append(CollationSpec::kCaseFirstField, CollationSpec::kCaseFirstOff);
            break;
        default:
            MONGO_UNREACHABLE;
    }

    builder.append(CollationSpec::kStrengthField, static_cast<int>(strength));
    builder.append(CollationSpec::kNumericOrderingField, numericOrdering);

    switch (alternate) {
        case CollationSpec::AlternateType::kNonIgnorable:
            builder.append(CollationSpec::kAlternateField, CollationSpec::kAlternateNonIgnorable);
            break;
        case CollationSpec::AlternateType::kShifted:
            builder.append(CollationSpec::kAlternateField, CollationSpec::kAlternateShifted);
            break;
        default:
            MONGO_UNREACHABLE;
    }

    switch (maxVariable) {
        case CollationSpec::MaxVariableType::kPunct:
            builder.append(CollationSpec::kMaxVariableField, CollationSpec::kMaxVariablePunct);
            break;
        case CollationSpec::MaxVariableType::kSpace:
            builder.append(CollationSpec::kMaxVariableField, CollationSpec::kMaxVariableSpace);
            break;
        default:
            MONGO_UNREACHABLE;
    }

    builder.append(CollationSpec::kNormalizationField, normalization);
    builder.append(CollationSpec::kBackwardsField, backwards);
    builder.append(CollationSpec::kVersionField, version);
    return builder.obj();
}

}  // namespace mongo
