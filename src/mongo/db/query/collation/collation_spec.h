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

#pragma once

#include <string>

#include "mongo/bson/bsonobj.h"

namespace mongo {

/**
 * A CollationSpec is a parsed representation of a user-provided collation BSONObj.
 */
struct CollationSpec {
    // Controls whether uppercase sorts before lowercase or vice versa.
    enum class CaseFirstType {
        // Sort uppercase before lowercase.
        kUpper,

        // Sort lowercase before uppercase.
        kLower,

        // Use default sorting behavior for the strength.
        kOff
    };

    // Controls the set of characteristics used to compare strings.
    enum class StrengthType {
        // Only consider base character differences.
        kPrimary = 1,

        // Additionally consider accent differences.
        kSecondary = 2,

        // Additionally consider case differences.
        kTertiary = 3,

        // Additionally consider punctuation and space differences. (If alternate=shifted, spaces
        // and punctuation are not considered base characters, and are only considered at this
        // strength.)
        kQuaternary = 4,

        // Equal Unicode point values.
        // E.g. Hebrew cantillation marks are only distinguished at this level.
        kIdentical = 5
    };

    // Controls whether spaces and punctuation are considered base characters.
    enum class AlternateType {
        // Spaces and punctuation are considered base characters.
        kNonIgnorable,

        // Spaces and punctuation are not considered base characters, and are only distinguished at
        // strength > 3.
        kShifted
    };

    // Controls which characters are affected by alternate=shifted.
    enum class MaxVariableType {
        // Punctuation and spaces are affected.
        kPunct,

        // Only spaces are affected
        kSpace
    };


    // Field name constants.
    static const char* kLocaleField;
    static const char* kCaseLevelField;
    static const char* kCaseFirstField;
    static const char* kStrengthField;
    static const char* kNumericOrderingField;
    static const char* kAlternateField;
    static const char* kMaxVariableField;
    static const char* kNormalizationField;
    static const char* kBackwardsField;
    static const char* kVersionField;

    // Field value constants.
    static const char* kSimpleBinaryComparison;
    static const char* kCaseFirstUpper;
    static const char* kCaseFirstLower;
    static const char* kCaseFirstOff;
    static const char* kAlternateNonIgnorable;
    static const char* kAlternateShifted;
    static const char* kMaxVariablePunct;
    static const char* kMaxVariableSpace;

    /**
     * Constructs a CollationSpec with no locale, where all other fields have their default values.
     */
    CollationSpec() = default;

    /**
     * Constructs a CollationSpec for the given locale, where all other fields have their default
     * values.
     */
    CollationSpec(std::string locale, std::string version)
        : localeID(std::move(locale)), version(std::move(version)) {}

    /**
     * Serializes this CollationSpec to its BSON format.
     */
    BSONObj toBSON() const;

    // A string such as "en_US", identifying the language, country, or other attributes of the
    // locale for this collation.
    // Required.
    std::string localeID;

    // Turns case sensitivity on at strength 1 or 2.
    bool caseLevel = false;

    CaseFirstType caseFirst = CaseFirstType::kOff;

    StrengthType strength = StrengthType::kTertiary;

    // Order numbers based on numerical order and not lexicographic order.
    bool numericOrdering = false;

    AlternateType alternate = AlternateType::kNonIgnorable;

    MaxVariableType maxVariable = MaxVariableType::kPunct;

    // Any language that uses multiple combining characters such as Arabic, ancient Greek, Hebrew,
    // Hindi, Thai or Vietnamese either requires Normalization Checking to be on, or the text to go
    // through a normalization process before collation.
    bool normalization = false;

    // Causes accent differences to be considered in reverse order, as it is done in the French
    // language.
    bool backwards = false;

    // Indicates the version of the collator. It is used to ensure that we do not mix versions by,
    // for example, constructing an index with one version of ICU and then attempting to use this
    // index with a server that is built against a newer ICU version.
    std::string version;
};

/**
 * Returns whether 'left' and 'right' are logically equivalent collations.
 */
inline bool operator==(const CollationSpec& left, const CollationSpec& right) {
    return ((left.localeID == right.localeID) && (left.caseLevel == right.caseLevel) &&
            (left.caseFirst == right.caseFirst) && (left.strength == right.strength) &&
            (left.numericOrdering == right.numericOrdering) &&
            (left.alternate == right.alternate) && (left.maxVariable == right.maxVariable) &&
            (left.normalization == right.normalization) && (left.backwards == right.backwards) &&
            (left.version == right.version));
}

inline bool operator!=(const CollationSpec& left, const CollationSpec& right) {
    return !(left == right);
}

}  // namespace mongo
