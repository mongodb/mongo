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
