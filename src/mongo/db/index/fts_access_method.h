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
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/btree_access_method_internal.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class FTSAccessMethod : public BtreeBasedAccessMethod {
    public:
        FTSAccessMethod(IndexDescriptor* descriptor);
        virtual ~FTSAccessMethod() { }

        // Not implemented:
        virtual Status newCursor(IndexCursor** out);

    private:
        // Implemented:
        virtual void getKeys(const BSONObj& obj, BSONObjSet* keys);

        fts::FTSSpec _ftsSpec;
    };

} //namespace mongo
