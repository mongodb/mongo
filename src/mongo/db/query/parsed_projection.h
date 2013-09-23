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

#include <string>
#include <vector>
#include "mongo/platform/unordered_set.h"

namespace mongo {

    /**
     * Superclass for all projections.  Projections are downcased to their specific implementation
     * and executed.
     */
    class ParsedProjection {
    public:
        virtual ~ParsedProjection() { }

        enum Type {
            FIND_SYNTAX,
        };

        virtual Type getType() const = 0;

        virtual string toString() const = 0;

        //
        // Properties of a projection.  These allow us to determine if the projection is covered or
        // not.
        //

        /**
         * Does the projection require the entire document?  If so, there must be a fetch before the
         * projection.
         */
        virtual bool requiresDocument() const = 0;

        /**
         * What fields does the projection require?
         */
        virtual const vector<string>& requiredFields() const = 0;
    };

    //
    // ParsedProjection implementations
    //
    class FindProjection : public ParsedProjection {
    public:
        Type getType() const {
            return ParsedProjection::FIND_SYNTAX;
        }

        string toString() const {
            // XXX FIXME
            return "";
        }

        bool requiresDocument() const {
            // If you're excluding fields, you must have something to exclude them from.
            if (_excludedFields.size() > 0) {
                verify(0 == _includedFields.size());
                return true;
            }

            // If you're including fields, they could come from an index.
            return false;
        }

        virtual const vector<string>& requiredFields() const {
            verify(0 == _excludedFields.size());
            return _includedFields;
        }

    private:
        // ProjectionParser constructs us.
        friend class ProjectionParser;

        // ProjectionExecutor reads the fields below.
        friend class ProjectionExecutor;

        // _id can be included/excluded separately and is by default included.
        bool _includeID;

        // Either you exclude certain fields...
        unordered_set<string> _excludedFields;

        // ...or you include other fields, which can be ordered.
        // UNITTEST 11738048
        vector<string> _includedFields;
    };

}  // namespace mongo
