// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
/**
 * Finds an element named "fieldName" in "object".
 *
 * Returns Status::OK() and sets "*outElement" to the found element on success.  Returns
 * ErrorCodes::NoSuchKey if there are no matches.
 */
Status bsonExtractField(const BSONObj& object, std::string_view fieldName, BSONElement* outElement);
StatusWith<BSONElement> bsonExtractField(const BSONObj& object, std::string_view fieldName);

/**
 * Finds an element named "fieldName" in "object".
 *
 * Returns Status::OK() and sets *outElement to the found element on success.  Returns
 * ErrorCodes::NoSuchKey if there are no matches for "fieldName", and ErrorCodes::TypeMismatch
 * if the type of the matching element is not "type".  For return values other than
 * Status::OK(), the resulting value of "*outElement" is undefined.
 */
Status bsonExtractTypedField(const BSONObj& object,
                             std::string_view fieldName,
                             BSONType type,
                             BSONElement* outElement);
StatusWith<BSONElement> bsonExtractTypedField(const BSONObj& object,
                                              std::string_view fieldName,
                                              BSONType type);

/**
 * Finds a bool-like element named "fieldName" in "object".
 *
 * Returns Status::OK() and sets *out to the found element's boolean value on success.  Returns
 * ErrorCodes::NoSuchKey if there are no matches for "fieldName", and ErrorCodes::TypeMismatch
 * if the type of the matching element is not Bool or a number type.  For return values other
 * than Status::OK(), the resulting value of "*out" is undefined.
 */
Status bsonExtractBooleanField(const BSONObj& object, std::string_view fieldName, bool* out);
StatusWith<bool> bsonExtractBooleanField(const BSONObj& object, std::string_view fieldName);

/**
 * If a field named "fieldName" is present, and is either a number or boolean type, stores the
 * truth value of the field into "*out".  If no field named "fieldName" is present, sets "*out"
 * to "defaultValue".  In these cases, returns Status::OK().
 *
 * If "fieldName" is present more than once, behavior is undefined.  If the found field is not a
 * boolean or number, returns ErrorCodes::TypeMismatch.
 */
Status bsonExtractBooleanFieldWithDefault(const BSONObj& object,
                                          std::string_view fieldName,
                                          bool defaultValue,
                                          bool* out);
StatusWith<bool> bsonExtractBooleanFieldWithDefault(const BSONObj& object,
                                                    std::string_view fieldName,
                                                    bool defaultValue);

/**
 * Finds an element named "fieldName" in "object" that represents an integral value.
 *
 * Returns Status::OK() and sets *out to the element's 64-bit integer value representation on
 * success.  Returns ErrorCodes::NoSuchKey if there are no matches for "fieldName".  Returns
 * ErrorCodes::TypeMismatch if the value of the matching element is not of a numeric type.
 * Returns ErrorCodes::BadValue if the value does not have an exact 64-bit integer
 * representation.  For return values other than Status::OK(), the resulting value of "*out" is
 * undefined.
 */
Status bsonExtractIntegerField(const BSONObj& object, std::string_view fieldName, long long* out);
StatusWith<long long> bsonExtractIntegerField(const BSONObj& object, std::string_view fieldName);

/**
 * If a field named "fieldName" is present and is a value of numeric type with an exact 64-bit
 * integer representation, returns that representation in *out and returns Status::OK().  If
 * there is no field named "fieldName", stores defaultValue into *out and returns Status::OK().
 * If the field is found, but has non-numeric type, returns ErrorCodes::TypeMismatch.  If the
 * value has numeric type, but cannot be represented as a 64-bit integer, returns
 * ErrorCodes::BadValue.
 */
Status bsonExtractIntegerFieldWithDefault(const BSONObj& object,
                                          std::string_view fieldName,
                                          long long defaultValue,
                                          long long* out);
StatusWith<long long> bsonExtractIntegerFieldWithDefault(const BSONObj& object,
                                                         std::string_view fieldName,
                                                         long long defaultValue);

/**
 * Finds an element named "fieldName" in "object" that represents a double-precision floating point
 * value.
 *
 * Returns Status::OK() and sets *out to the element's double floating point value representation on
 * success. Returns ErrorCodes::NoSuchKey if there are no matches for "fieldName". Returns
 * ErrorCodes::TypeMismatch if the value of the matching element is not of a numeric type. Returns
 * ErrorCodes::BadValue if the value does not have an exact floating point number representation.
 * For return values other than Status::OK(), the resulting value of "*out" is undefined.
 */
Status bsonExtractDoubleField(const BSONObj& object, std::string_view fieldName, double* out);
StatusWith<double> bsonExtractDoubleField(const BSONObj& object, std::string_view fieldName);

/**
 * Finds a string-typed element named "fieldName" in "object" and stores its value in "out".
 *
 * Returns Status::OK() and sets *out to the found element's std::string value on success.  Returns
 * ErrorCodes::NoSuchKey if there are no matches for "fieldName", and ErrorCodes::TypeMismatch
 * if the type of the matching element is not String.  For return values other than
 * Status::OK(), the resulting value of "*out" is undefined.
 */
Status bsonExtractStringField(const BSONObj& object, std::string_view fieldName, std::string* out);
StatusWith<std::string> bsonExtractStringField(const BSONObj& object, std::string_view fieldName);

/**
 * If a field named "fieldName" is present, and is a string, stores the value of the field into
 * "*out".  If no field named fieldName is present, sets "*out" to "defaultValue".  In these
 * cases, returns Status::OK().
 *
 * If "fieldName" is present more than once, behavior is undefined.  If the found field is not a
 * string, returns ErrorCodes::TypeMismatch.
 */
Status bsonExtractStringFieldWithDefault(const BSONObj& object,
                                         std::string_view fieldName,
                                         std::string_view defaultValue,
                                         std::string* out);
StatusWith<std::string> bsonExtractStringFieldWithDefault(const BSONObj& object,
                                                          std::string_view fieldName,
                                                          std::string_view defaultValue);

/**
 * Finds an Timestamp-typed element named "fieldName" in "object" and stores its value in "out".
 *
 * Returns Status::OK() and sets *out to the found element's Timestamp value on success. Returns
 * ErrorCodes::NoSuchKey if there are no matches for "fieldName", and ErrorCodes::TypeMismatch
 * if the type of the matching element is not Timestamp.  For return values other than
 * Status::OK(), the resulting value of "*out" is undefined.
 */
Status bsonExtractTimestampField(const BSONObj& object, std::string_view fieldName, Timestamp* out);
StatusWith<Timestamp> bsonExtractTimestampField(const BSONObj& object, std::string_view fieldName);

/**
 * Finds an OID-typed element named "fieldName" in "object" and stores its value in "out".
 *
 * Returns Status::OK() and sets *out to the found element's OID value on success.  Returns
 * ErrorCodes::NoSuchKey if there are no matches for "fieldName", and ErrorCodes::TypeMismatch
 * if the type of the matching element is not OID.  For return values other than Status::OK(),
 * the resulting value of "*out" is undefined.
 */
Status bsonExtractOIDField(const BSONObj& object, std::string_view fieldName, OID* out);
StatusWith<OID> bsonExtractOIDField(const BSONObj& object, std::string_view fieldName);
}  // namespace mongo
