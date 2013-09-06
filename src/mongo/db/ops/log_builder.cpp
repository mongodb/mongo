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

#include "mongo/db/ops/log_builder.h"

namespace mongo {

    using mutablebson::Document;
    using mutablebson::Element;

    namespace {
        const char kSet[] = "$set";
        const char kUnset[] = "$unset";
    } // namespace

    inline Status LogBuilder::addToSection(Element newElt,
                                           Element* section,
                                           const char* sectionName) {

        // If we don't already have this section, try to create it now.
        if (!section->ok()) {

            // If we already have object replacement data, we can't also have section entries.
            if (hasObjectReplacement())
                return Status(
                    ErrorCodes::IllegalOperation,
                    "LogBuilder: Invalid attempt to add a $set/$unset entry"
                    "to a log with an existing object replacement");

            Document& doc = _logRoot.getDocument();

            // We should not already have an element with the section name under the root.
            dassert(_logRoot[sectionName] == doc.end());

            // Construct a new object element to represent this section in the log.
            const Element newElement = doc.makeElementObject(sectionName);
            if (!newElement.ok())
                return Status(ErrorCodes::InternalError,
                              "LogBuilder: failed to construct Object Element for $set/$unset");

            // Enqueue the new section under the root, and record it as our out parameter.
            Status result = _logRoot.pushBack(newElement);
            if (!result.isOK())
                return result;
            *section = newElement;

            // Invalidate attempts to add an object replacement, now that we have a named
            // section under the root.
            _objectReplacementAccumulator = doc.end();
        }

        // Whatever transpired, we should now have an ok accumulator for the section, and not
        // have a replacement accumulator.
        dassert(section->ok());
        dassert(!_objectReplacementAccumulator.ok());

        // Enqueue the provided element to the section and propagate the result.
        return section->pushBack(newElt);
    }

    Status LogBuilder::addToSets(Element elt) {
        return addToSection(elt, &_setAccumulator, kSet);
    }

    Status LogBuilder::addToUnsets(Element elt) {
        return addToSection(elt, &_unsetAccumulator, kUnset);
    }

    Status LogBuilder::getReplacementObject(Element* outElt) {

        // If the replacement accumulator is not ok, we must have started a $set or $unset
        // already, so an object replacement is not permitted.
        if (!_objectReplacementAccumulator.ok()) {
            dassert(_setAccumulator.ok() || _unsetAccumulator.ok());
            return Status(
                ErrorCodes::IllegalOperation,
                "LogBuilder: Invalid attempt to obtain the object replacement slot "
                "for a log containing $set or $unset entries");
        }

        if (hasObjectReplacement())
            return Status(
                ErrorCodes::IllegalOperation,
                "LogBuilder: Invalid attempt to acquire the replacement object "
                "in a log with existing object replacement data");

        // OK to enqueue object replacement items.
        *outElt = _objectReplacementAccumulator;
        return Status::OK();
    }

    inline bool LogBuilder::hasObjectReplacement() const {
        if (!_objectReplacementAccumulator.ok())
            return false;

        dassert(!_setAccumulator.ok());
        dassert(!_unsetAccumulator.ok());

        return _objectReplacementAccumulator.hasChildren();
    }

} // namespace mongo
