/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/jsobj.h"

namespace mongo {

/**
 * "Types" are the interface to a known data structure that will be deserialized from BSON.
 */
class BSONSerializable {
public:
    virtual ~BSONSerializable() {}

    /**
     * Returns true if all the mandatory fields are present and have valid
     * representations. Otherwise returns false and fills in the optional 'errMsg' string.
     */
    virtual bool isValid(std::string* errMsg) const = 0;

    /**
     * Clears and populates the internal state using the 'source' BSON object if the
     * latter contains valid values. Otherwise sets errMsg and returns false.
     */
    virtual bool parseBSON(const BSONObj& source, std::string* errMsg) = 0;

    /** Clears the internal state. */
    virtual void clear() = 0;

    /** Returns a std::string representation of the current internal state. */
    virtual std::string toString() const = 0;
};

/**
 * Generic implementation which accepts and stores any BSON object
 *
 * Generally this should only be used for compatibility reasons - newer requests should be
 * fully typed.
 */
class RawBSONSerializable : public BSONSerializable {
public:
    RawBSONSerializable() {}

    explicit RawBSONSerializable(const BSONObj& raw) : _raw(raw) {}

    virtual ~RawBSONSerializable() {}

    virtual bool isValid(std::string* errMsg) const {
        return true;
    }

    virtual BSONObj toBSON() const {
        return _raw;
    }

    virtual bool parseBSON(const BSONObj& source, std::string* errMsg) {
        _raw = source.getOwned();
        return true;
    }

    virtual void clear() {
        _raw = BSONObj();
    }

    virtual std::string toString() const {
        return toBSON().toString();
    }

private:
    BSONObj _raw;
};


}  // namespace mongo
