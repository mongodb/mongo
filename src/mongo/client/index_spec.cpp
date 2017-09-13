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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/index_spec.h"

#include "mongo/client/dbclientinterface.h"

namespace mongo {

const char IndexSpec::kIndexValText[] = "text";
const char IndexSpec::kIndexValGeo2D[] = "2d";
const char IndexSpec::kIndexValGeoHaystack[] = "geoHaystack";
const char IndexSpec::kIndexValGeo2DSphere[] = "2dsphere";
const char IndexSpec::kIndexValHashed[] = "hashed";

namespace {

const int kIndexTypeNumbers[] = {IndexSpec::kIndexValAscending, IndexSpec::kIndexValDescending};

const char* const kIndexTypeStrings[] = {NULL,
                                         NULL,
                                         IndexSpec::kIndexValText,
                                         IndexSpec::kIndexValGeo2D,
                                         IndexSpec::kIndexValGeoHaystack,
                                         IndexSpec::kIndexValGeo2DSphere,
                                         IndexSpec::kIndexValHashed};

const char kDuplicateKey[] = "duplicate key added to index descriptor";
const char kDuplicateOption[] = "duplicate option added to index descriptor";

}  // namespace

IndexSpec::IndexSpec() : _dynamicName(true) {}

IndexSpec& IndexSpec::addKey(const StringData& field, IndexType type) {
    uassert(ErrorCodes::InvalidOptions, kDuplicateKey, !_keys.asTempObj().hasField(field));
    if (type <= kIndexTypeDescending)
        _keys.append(field, kIndexTypeNumbers[type]);
    else
        _keys.append(field, kIndexTypeStrings[type]);
    _rename();
    return *this;
}

IndexSpec& IndexSpec::addKey(const BSONElement& fieldAndType) {
    uassert(ErrorCodes::InvalidOptions,
            kDuplicateKey,
            !_keys.asTempObj().hasField(fieldAndType.fieldName()));
    _keys.append(fieldAndType);
    _rename();
    return *this;
}

IndexSpec& IndexSpec::addKeys(const KeyVector& keys) {
    KeyVector::const_iterator where = keys.begin();
    const KeyVector::const_iterator end = keys.end();
    for (; where != end; ++where)
        addKey(where->first, where->second);
    return *this;
}

IndexSpec& IndexSpec::addKeys(const BSONObj& keys) {
    BSONObjIterator iter(keys);
    while (iter.more())
        addKey(iter.next());
    return *this;
}

IndexSpec& IndexSpec::background(bool value) {
    uassert(
        ErrorCodes::InvalidOptions, kDuplicateOption, !_options.asTempObj().hasField("background"));
    _options.append("background", value);
    return *this;
}

IndexSpec& IndexSpec::unique(bool value) {
    uassert(ErrorCodes::InvalidOptions, kDuplicateOption, !_options.asTempObj().hasField("unique"));
    _options.append("unique", value);
    return *this;
}

IndexSpec& IndexSpec::name(const StringData& value) {
    _name = value.toString();
    _dynamicName = false;
    return *this;
}

IndexSpec& IndexSpec::dropDuplicates(bool value) {
    uassert(
        ErrorCodes::InvalidOptions, kDuplicateOption, !_options.asTempObj().hasField("dropDups"));
    _options.append("dropDups", value);
    return *this;
}

IndexSpec& IndexSpec::sparse(bool value) {
    uassert(ErrorCodes::InvalidOptions, kDuplicateOption, !_options.asTempObj().hasField("sparse"));
    _options.append("sparse", value);
    return *this;
}

IndexSpec& IndexSpec::expireAfterSeconds(int value) {
    uassert(ErrorCodes::InvalidOptions,
            kDuplicateOption,
            !_options.asTempObj().hasField("expireAfterSeconds"));
    _options.append("expireAfterSeconds", value);
    return *this;
}

IndexSpec& IndexSpec::version(int value) {
    uassert(ErrorCodes::InvalidOptions, kDuplicateOption, !_options.asTempObj().hasField("v"));
    _options.append("v", value);
    return *this;
}

IndexSpec& IndexSpec::textWeights(const BSONObj& value) {
    uassert(
        ErrorCodes::InvalidOptions, kDuplicateOption, !_options.asTempObj().hasField("weights"));
    _options.append("weights", value);
    return *this;
}

IndexSpec& IndexSpec::textDefaultLanguage(const StringData& value) {
    uassert(ErrorCodes::InvalidOptions,
            kDuplicateOption,
            !_options.asTempObj().hasField("default_language"));
    _options.append("default_language", value);
    return *this;
}

IndexSpec& IndexSpec::textLanguageOverride(const StringData& value) {
    uassert(ErrorCodes::InvalidOptions,
            kDuplicateOption,
            !_options.asTempObj().hasField("language_override"));
    _options.append("language_override", value);
    return *this;
}

IndexSpec& IndexSpec::textIndexVersion(int value) {
    uassert(ErrorCodes::InvalidOptions,
            kDuplicateOption,
            !_options.asTempObj().hasField("textIndexVersion"));
    _options.append("textIndexVersion", value);
    return *this;
}

IndexSpec& IndexSpec::geo2DSphereIndexVersion(int value) {
    uassert(ErrorCodes::InvalidOptions,
            kDuplicateOption,
            !_options.asTempObj().hasField("2dsphereIndexVersion"));
    _options.append("2dsphereIndexVersion", value);
    return *this;
}

IndexSpec& IndexSpec::geo2DBits(int value) {
    uassert(ErrorCodes::InvalidOptions, kDuplicateOption, !_options.asTempObj().hasField("bits"));
    _options.append("bits", value);
    return *this;
}

IndexSpec& IndexSpec::geo2DMin(double value) {
    uassert(ErrorCodes::InvalidOptions, kDuplicateOption, !_options.asTempObj().hasField("min"));
    _options.append("min", value);
    return *this;
}

IndexSpec& IndexSpec::geo2DMax(double value) {
    uassert(ErrorCodes::InvalidOptions, kDuplicateOption, !_options.asTempObj().hasField("max"));
    _options.append("max", value);
    return *this;
}

IndexSpec& IndexSpec::geoHaystackBucketSize(double value) {
    uassert(
        ErrorCodes::InvalidOptions, kDuplicateOption, !_options.asTempObj().hasField("bucketSize"));
    _options.append("bucketSize", value);
    return *this;
}

IndexSpec& IndexSpec::addOption(const BSONElement& option) {
    uassert(ErrorCodes::InvalidOptions,
            kDuplicateOption,
            !_options.asTempObj().hasField(option.fieldName()));
    _options.append(option);
    return *this;
}

IndexSpec& IndexSpec::addOptions(const BSONObj& options) {
    BSONObjIterator iter(options);
    while (iter.more())
        addOption(iter.next());
    return *this;
}

std::string IndexSpec::name() const {
    return _name;
}

BSONObj IndexSpec::toBSON() const {
    BSONObjBuilder bob;
    bob.append("name", name());
    bob.append("key", _keys.asTempObj());
    bob.appendElements(_options.asTempObj());
    return bob.obj();
}

void IndexSpec::_rename() {
    if (!_dynamicName)
        return;
    _name = DBClientBase::genIndexName(_keys.asTempObj());
}

}  // namespace mongo
