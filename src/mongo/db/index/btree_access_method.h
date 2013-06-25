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

#include <boost/scoped_ptr.hpp>

#include "mongo/base/status.h"
#include "mongo/db/btree.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index/btree_access_method_internal.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class BtreeInterface;
    class IndexCursor;
    class IndexDescriptor;

    /**
     * The IndexAccessMethod for a Btree index.
     * Any index created with {field: 1} or {field: -1} uses this.
     */
    class BtreeAccessMethod : public BtreeBasedAccessMethod {
    public:
        // Every Btree-based index needs these.  We put them in the BtreeBasedAccessMethod
        // superclass and subclasses (like this) can use them.
        using BtreeBasedAccessMethod::_descriptor;
        using BtreeBasedAccessMethod::_interface;
        using BtreeBasedAccessMethod::_ordering;

        BtreeAccessMethod(IndexDescriptor* descriptor);
        virtual ~BtreeAccessMethod() { }

        virtual Status newCursor(IndexCursor** out);

    private:
        virtual void getKeys(const BSONObj& obj, BSONObjSet* keys);

        // Our keys differ for V0 and V1.
        scoped_ptr<BtreeKeyGenerator> _keyGenerator;
    };

}  // namespace mongo
