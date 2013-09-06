/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * This class represents the layout and contents of documents contained in the
     * config.tags collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     BSONObj query = QUERY(TagsType::exampleField("exampleFieldName"));
     *     exampleDoc = conn->findOne(TagsType::ConfigNS, query);
     *
     *     // Process the response.
     *     TagsType exampleType;
     *     string errMsg;
     *     if (!exampleType.parseBSON(exampleDoc, &errMsg) || !exampleType.isValid(&errMsg)) {
     *         // Can't use 'exampleType'. Take action.
     *     }
     *     // use 'exampleType'
     *
     */
    class TagsType {
        MONGO_DISALLOW_COPYING(TagsType);
    public:

        //
        // schema declarations
        //

        // Name of the tags collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the tags collection type.
        static const BSONField<std::string> ns;
        static const BSONField<std::string> tag;
        static const BSONField<BSONObj> min;
        static const BSONField<BSONObj> max;

        //
        // tags type methods
        //

        TagsType();
        ~TagsType();

        /**
         * Returns true if all the mandatory fields are present and have valid
         * representations. Otherwise returns false and fills in the optional 'errMsg' string.
         */
        bool isValid(std::string* errMsg) const;

        /**
         * Returns the BSON representation of the entry.
         */
        BSONObj toBSON() const;

        /**
         * Clears and populates the internal state using the 'source' BSON object if the
         * latter contains valid values. Otherwise sets errMsg and returns false.
         */
        bool parseBSON(const BSONObj& source, std::string* errMsg);

        /**
         * Clears the internal state.
         */
        void clear();

        /**
         * Copies all the fields present in 'this' to 'other'.
         */
        void cloneTo(TagsType* other) const;

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        // Mandatory Fields
        void setNS(const StringData& ns) {
            _ns = ns.toString();
            _isNsSet = true;
        }

        void unsetNS() { _isNsSet = false; }

        bool isNSSet() const { return _isNsSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string& getNS() const {
            dassert(_isNsSet);
            return _ns;
        }

        void setTag(const StringData& tag) {
            _tag = tag.toString();
            _isTagSet = true;
        }

        void unsetTag() { _isTagSet = false; }

        bool isTagSet() const { return _isTagSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string& getTag() const {
            dassert(_isTagSet);
            return _tag;
        }

        void setMin(const BSONObj& min) {
            _min = min.getOwned();
            _isMinSet = true;
        }

        void unsetMin() { _isMinSet = false; }

        bool isMinSet() const { return _isMinSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const BSONObj getMin() const {
            dassert(_isMinSet);
            return _min;
        }

        void setMax(const BSONObj& max) {
            _max = max.getOwned();
            _isMaxSet = true;
        }

        void unsetMax() { _isMaxSet = false; }

        bool isMaxSet() const { return _isMaxSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const BSONObj getMax() const {
            dassert(_isMaxSet);
            return _max;
        }

        // Optional Fields

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _ns;     // (M)  namespace this tag is for
        bool _isNsSet;
        std::string _tag;     // (M)  tag name
        bool _isTagSet;
        BSONObj _min;     // (M)  first key of the tag, including
        bool _isMinSet;
        BSONObj _max;     // (M)  last key of the tag, non-including
        bool _isMaxSet;
    };

} // namespace mongo
