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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <mongo/base/status.h>
#include <mongo/bson/mutable/document.h>

namespace mongo {

    /** LogBuilder abstracts away some of the details of producing a properly constructed oplog
     *  update entry. It manages separate regions into which it accumulates $set and $unset
     *  operations, and distinguishes object replacement style oplog generation from
     *  $set/$unset style generation and prevents admixture.
     */
    class LogBuilder {
    public:
        /** Construct a new LogBuilder. Log entries will be recorded as new children under the
         *  'logRoot' Element, which must be of type mongo::Object and have no children.
         */
        inline LogBuilder(mutablebson::Element logRoot)
            : _logRoot(logRoot)
            , _objectReplacementAccumulator(_logRoot)
            , _setAccumulator(_logRoot.getDocument().end())
            , _unsetAccumulator(_setAccumulator) {
            dassert(logRoot.isType(mongo::Object));
            dassert(!logRoot.hasChildren());
        }

        /** Return the Document to which the logging root belongs. */
        inline mutablebson::Document& getDocument() {
            return _logRoot.getDocument();
        }

        /** Add the given Element as a new entry in the '$set' section of the log. If a $set
         *  section does not yet exist, it will be created. If this LogBuilder is currently
         *  configured to contain an object replacement, the request to add to the $set section
         *  will return an Error.
         */
        Status addToSets(mutablebson::Element elt);

        /** Add the given Element as a new entry in the '$unset' section of the log. If an
         *  '$unset' section does not yet exist, it will be created. If this LogBuilder is
         *  currently configured to contain an object replacement, the request to add to the
         *  $unset section will return an Error.
         */
        Status addToUnsets(mutablebson::Element elt);

        /** Obtain, via the out parameter 'outElt', a pointer to the mongo::Object type Element
         *  to which the components of an object replacement should be recorded. It is an error
         *  to call this if any Elements have been added by calling either addToSets or
         *  addToUnsets, and attempts to do so will return a non-OK Status. Similarly, if there
         *  is already object replacement data recorded for this log, the call will fail.
         */
        Status getReplacementObject(mutablebson::Element* outElt);

    private:
        // Returns true if the object replacement accumulator is valid and has children, false
        // otherwise.
        inline bool hasObjectReplacement() const;

        inline Status addToSection(
            mutablebson::Element newElt,
            mutablebson::Element* section,
            const char* sectionName);

        mutablebson::Element _logRoot;
        mutablebson::Element _objectReplacementAccumulator;
        mutablebson::Element _setAccumulator;
        mutablebson::Element _unsetAccumulator;
    };

} // namespace mongo
