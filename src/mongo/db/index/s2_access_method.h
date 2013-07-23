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
*/

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/index/btree_access_method_internal.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class IndexCursor;
    struct S2IndexingParams;

    class S2AccessMethod : public BtreeBasedAccessMethod {
    public:
        using BtreeBasedAccessMethod::_descriptor;

        S2AccessMethod(IndexDescriptor* descriptor);
        virtual ~S2AccessMethod() { }

        virtual Status newCursor(IndexCursor** out);

    private:
        friend class Geo2dFindNearCmd;
        const S2IndexingParams& getParams() const { return _params; }

        virtual void getKeys(const BSONObj& obj, BSONObjSet* keys);

        // getKeys calls the helper methods below.
        void getGeoKeys(const BSONObj& document, const BSONElementSet& elements,
                        BSONObjSet* out) const;
        void getLiteralKeys(const BSONElementSet& elements, BSONObjSet* out) const;
        void getLiteralKeysArray(const BSONObj& obj, BSONObjSet* out) const;
        void getOneLiteralKey(const BSONElement& elt, BSONObjSet *out) const;

        S2IndexingParams _params;
    };

}  // namespace mongo
