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

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "mongo/db/jsobj.h"

namespace mongo {

class StringData;

class IndexSpec {
public:
    // An enumeration of symbolic names for index types.
    enum IndexType {
        kIndexTypeAscending,
        kIndexTypeDescending,
        kIndexTypeText,
        kIndexTypeGeo2D,
        kIndexTypeGeoHaystack,
        kIndexTypeGeo2DSphere,
        kIndexTypeHashed,
    };

    // The values to be encoded in BSON for each type of index.
    static const int kIndexValAscending = 1;
    static const int kIndexValDescending = -1;
    static const char kIndexValText[];
    static const char kIndexValGeo2D[];
    static const char kIndexValGeoHaystack[];
    static const char kIndexValGeo2DSphere[];
    static const char kIndexValHashed[];

    /** Create a new IndexSpec. */
    IndexSpec();

    //
    // Methods for adding keys. Methods on this class will prevent you from adding a given
    // key multiple times. Constraints on the validity of compound indexes are not enforced
    // here.
    //

    /** Add a new component, by default ascending, field to index. */
    IndexSpec& addKey(const StringData& field, IndexType type = kIndexTypeAscending);

    /** Add a component to this index. The field name of the element is used as the field
     *  name to index. The value of the element is the index type. This method exists to
     *  accomodate building future index types for which the enumeration value has not yet
     *  been extended.
     */
    IndexSpec& addKey(const BSONElement& fieldAndType);

    /** Add all components in the provided key vector to the index descriptor. */
    typedef std::vector<std::pair<std::string, IndexType>> KeyVector;
    IndexSpec& addKeys(const KeyVector& keys);

    /** Add all keys from the provided object to the index descriptor. */
    IndexSpec& addKeys(const BSONObj& keys);


    //
    // Methods for adding options. As for keys, duplicated settings are checked and will
    // raise an error.
    //

    //
    // Common index options
    //

    /** Controls whether this index should be built in the foreground or background. By
     *  default indexes are built in the foreground.
     */
    IndexSpec& background(bool value = true);

    /** Set whether or not this index should enforce uniqueness. By default, uniqueness is
     *  not enforced.
     */
    IndexSpec& unique(bool value = true);


    /** Set the name for this index. If not set, a name will be automatically generated. */
    IndexSpec& name(const StringData& name);

    /** Sets whether duplicates detected while indexing should be dropped. By default,
     *  duplicates are not dropped.
     */
    IndexSpec& dropDuplicates(bool value = true);

    /** Sets whether or not this index should be sparse. By default, indexes are not
     *  sparse.
     */
    IndexSpec& sparse(bool value = true);

    /** Enables time-to-live semantics for this index with the specified lifetime in
     *  seconds. Note that the indexed field must be of type UTC datetime for this option
     *  to work correctly.
     */
    IndexSpec& expireAfterSeconds(int value);

    /** Explicitly request an index of the given version. As of MongoDB 2.6, the only accepted
     *  values are 0 or 1. Versions 2.0 and later default to '1'. Do not set this option except
     *  in unusual circumstances.
     */
    IndexSpec& version(int value);


    //
    // Text options
    //

    /** Sets the 'weights' document for a text index. */
    IndexSpec& textWeights(const BSONObj& value);

    /** Sets the default language for a text index. */
    IndexSpec& textDefaultLanguage(const StringData& value);

    /** Sets the name of the field containing the language override in a text index. */
    IndexSpec& textLanguageOverride(const StringData& value);

    /** Sets the version of the text index to use. MongoDB 2.4 only supports version
     *  '1'. If not otherwise specified, MongoDB 2.6 defaults to version 2.
     */
    IndexSpec& textIndexVersion(int value);


    //
    // 2D Sphere Options
    //

    /** Sets the version of the 2D sphere index to use. MongoDB 2.4 only supports version
     *  '1'. If not otherwise specified, MongoDB 2.6 defaults to version 2.
     */
    IndexSpec& geo2DSphereIndexVersion(int value);


    //
    // Geo2D Options
    //

    /** Sets the number of bits of precision for geohash. */
    IndexSpec& geo2DBits(int value);

    /** Sets the minimum value for keys in a geo2d index. */
    IndexSpec& geo2DMin(double value);

    /** Sets the maximum value for keys in a geo2d index. */
    IndexSpec& geo2DMax(double value);


    //
    // Geo Haystack Options
    //

    /** Sets the bucket size for haystack indexes. */
    IndexSpec& geoHaystackBucketSize(double value);


    //
    // Support for adding generic options. This is here so that if new index options
    // become available on the server those options can be set independently of the
    // named methods above.
    //

    /** Adds another option verbatim. */
    IndexSpec& addOption(const BSONElement& option);

    /** Adds the provided options verbatim. */
    IndexSpec& addOptions(const BSONObj& options);

    /** Get a copy of the current name for this index. If a name was provided to the
     *  constructor, a copy of this name is returned. Otherwise, the current auto-generated
     *  name given the set of indexes will be returned. Note that this is a copy:
     *  subsequent changes to the indexed fields will not affect the value returned here,
     *  and you must call 'name' again to obtain the updated value.
     */
    std::string name() const;

    /** Return a BSONObj that captures the selected index keys and options. */
    BSONObj toBSON() const;

private:
    void _rename();

    std::string _name;
    bool _dynamicName;

    mutable BSONObjBuilder _keys;
    mutable BSONObjBuilder _options;
};

}  // namespace mongo
