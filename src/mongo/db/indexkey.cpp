// index_key.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "pch.h"
#include "namespace-inl.h"
#include "index.h"
#include "background.h"
#include "../util/stringutils.h"
#include "mongo/util/mongoutils/str.h"
#include "../util/text.h"
#include "mongo/db/client.h"
#include "mongo/db/database.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/queryutil.h"

namespace mongo {

    /** old (<= v1.8) : 0
     1 is new version
     */
    const int DefaultIndexVersionNumber = 1;
    
    void IndexSpec::_init() {
        verify( keyPattern.objsize() );

        // some basics
        _nFields = keyPattern.nFields();
        _sparse = info["sparse"].trueValue();
        uassert( 13529 , "sparse only works for single field keys" , ! _sparse || _nFields );


        {
            // build _nullKey

            BSONObjBuilder b;
            BSONObjIterator i( keyPattern );

            while( i.more() ) {
                BSONElement e = i.next();
                _fieldNames.push_back( e.fieldName() );
                _fixed.push_back( BSONElement() );
                b.appendNull( "" );
            }
            _nullKey = b.obj();
        }

        {
            // _nullElt
            BSONObjBuilder b;
            b.appendNull( "" );
            _nullObj = b.obj();
            _nullElt = _nullObj.firstElement();
        }

        {
            // _undefinedElt
            BSONObjBuilder b;
            b.appendUndefined( "" );
            _undefinedObj = b.obj();
            _undefinedElt = _undefinedObj.firstElement();
        }

        _finishedInit = true;
    }

    string IndexSpec::getTypeName() const {
        return CatalogHack::findPluginName(_details->keyPattern());
    }

    string IndexSpec::toString() const {
        stringstream s;
        s << "IndexSpec @ " << hex << this << dec << ", "
          << "Details @ " << hex << _details << dec << ", "
          << "Type: " << getTypeName() << ", "
          << "nFields: " << _nFields << ", "
          << "KeyPattern: " << keyPattern << ", "
          << "Info: " << info;
        return s.str();
    }
    
    int IndexSpec::indexVersion() const {
        if ( !info.hasField( "v" ) ) {
            return DefaultIndexVersionNumber;
        }
        return IndexDetails::versionForIndexObj( info );
    }    

}
