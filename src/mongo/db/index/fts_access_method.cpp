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

#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/fts/fts_index_format.h"

namespace mongo {

    FTSAccessMethod::FTSAccessMethod(IndexDescriptor* descriptor)
        : BtreeBasedAccessMethod(descriptor), _ftsSpec(descriptor->infoObj()) { }

    void FTSAccessMethod::getKeys(const BSONObj& obj, BSONObjSet* keys) {
        fts::FTSIndexFormat::getKeys(_ftsSpec, obj, keys);
    }

    Status FTSAccessMethod::newCursor(IndexCursor** out) {
        return Status::OK();
    }

}  // namespace mongo
