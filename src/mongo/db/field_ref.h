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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/scoped_array.hpp>
#include <iosfwd>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"

namespace mongo {

    /**
     * A FieldPath represents a path in a document, starting from the root. The path
     * is made of "field parts" separated by dots. The class provides an efficient means to
     * "split" the dotted fields in its parts, but no validation is done.
     *
     * Any field part may be replaced, after the "original" field reference was parsed. Any
     * part can be accessed through a StringData object.
     *
     * The class is not thread safe.
     */
    class FieldRef {
        MONGO_DISALLOW_COPYING(FieldRef);
    public:
        FieldRef() : _size(0) {}

        /**
         * Field parts accessed through getPart() calls no longer would be valid, after the
         * destructor ran.
         */
        ~FieldRef() {}

        /**
         * Builds a field path out of each field part in 'dottedField'.
         */
        void parse(const StringData& dottedField);

        /**
         * Sets the 'i-th' field part to point to 'part'. Assumes i < size(). Behavior is
         * undefined otherwise.
         */
        void setPart(size_t i, const StringData& part);

        /**
         * Returns the 'i-th' field part. Assumes i < size(). Behavior is undefined otherwise.
         */
        StringData getPart(size_t i) const;

        /**
         * Returns true when 'this' FieldRef is a prefix of 'other'. Equality is not considered
         * a prefix.
         */
        bool isPrefixOf( const FieldRef& other ) const;

        /**
         * Returns the number of field parts in the prefix that 'this' and 'other' share.
         */
        size_t commonPrefixSize( const FieldRef& other ) const;

        /**
         * Returns a copy of the full dotted field in its current state (i.e., some parts may
         * have been replaced since the parse() call).
         */
        std::string dottedField( size_t offset = 0 ) const;

        /**
         * Compares the full dotted path represented by this FieldRef to other
         */
        bool equalsDottedField( const StringData& other ) const;

        /**
         * Return 0 if 'this' is equal to 'other' lexicographically, -1 if is it less than or
         * +1 if it is greater than.
         */
        int compare( const FieldRef& other ) const;

        /**
         * Resets the internal state. See note in parse() call.
         */
        void clear();

        //
        // accessors
        //

        /**
         * Returns the number of parts in this FieldRef.
         */
        size_t numParts() const { return _size; }

        /**
         * Returns the number of fields parts that were replaced so far. Replacing the same
         * fields several times only counts for 1.
         */
        size_t numReplaced() const;

    private:
        // Dotted fields are most often not longer than four parts. We use a mixed structure
        // here that will not require any extra memory allocation when that is the case. And
        // handle larger dotted fields if it is. The idea is not to penalize the common case
        // with allocations.
        static const size_t kReserveAhead = 4;

        /** Converts the field part index to the variable part equivalent */
        size_t getIndex(size_t i) const { return i-kReserveAhead; }

        /**
         * Returns the new number of parts after appending 'part' to this field path. It
         * assumes that 'part' is pointing to an internally allocated area.
         */
        size_t appendPart(const StringData& part);

        // number of field parts stored
        size_t _size;

        // first kResevedAhead field components
        StringData _fixed[kReserveAhead];

         // remaining field components
        std::vector<StringData> _variable;

        // cached dotted name
        std::string _dotted;

        // back memory added with the setPart call pointed to by _fized and _variable
        std::vector<std::string> _replacements;
    };

    inline bool operator==(const FieldRef& lhs, const FieldRef& rhs) {
        return lhs.compare(rhs) == 0;
    }

    inline bool operator!=(const FieldRef& lhs, const FieldRef& rhs) {
        return lhs.compare(rhs) != 0;
    }

    inline bool operator<(const FieldRef& lhs, const FieldRef& rhs) {
        return lhs.compare(rhs) < 0;
    }

    inline bool operator<=(const FieldRef& lhs, const FieldRef& rhs) {
        return lhs.compare(rhs) <= 0;
    }

    inline bool operator>(const FieldRef& lhs, const FieldRef& rhs) {
        return lhs.compare(rhs) > 0;
    }

    inline bool operator>=(const FieldRef& lhs, const FieldRef& rhs) {
        return lhs.compare(rhs) >= 0;
    }

    std::ostream& operator<<(std::ostream& stream, const FieldRef& value);

} // namespace mongo
