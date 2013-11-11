/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/s/write_ops/dbclient_safe_writer.h"

#include "mongo/s/version_manager.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    void DBClientSafeWriter::safeWrite( DBClientBase* conn,
                                        const BatchItemRef& itemRef,
                                        LastError* error ) {

        const BatchedCommandRequest* request = itemRef.getRequest();

        try {

            // Default settings for checkShardVersion
            const bool authoritative = false;
            const int tryNum = 1;

            // We need to set our version using setShardVersion, managed by checkShardVersionCB
            versionManager.checkShardVersionCB( conn,
                                                request->getTargetingNS(),
                                                authoritative,
                                                tryNum );

            if ( request->getBatchType() == BatchedCommandRequest::BatchType_Insert ) {
                conn->insert( request->getNS(),
                              request->getInsertRequest()->getDocumentsAt( itemRef.getItemIndex() ),
                              0 );
            }
            else if ( request->getBatchType() == BatchedCommandRequest::BatchType_Update ) {
                const BatchedUpdateDocument* update =
                    request->getUpdateRequest()->getUpdatesAt( itemRef.getItemIndex() );
                conn->update( request->getNS(),
                              update->getQuery(),
                              update->getUpdateExpr(),
                              update->getUpsert(),
                              update->getMulti() );
            }
            else {
                dassert( request->getBatchType() == BatchedCommandRequest::BatchType_Delete );
                const BatchedDeleteDocument* deleteDoc =
                    request->getDeleteRequest()->getDeletesAt( itemRef.getItemIndex() );
                conn->remove( request->getNS(),
                              deleteDoc->getQuery(),
                              deleteDoc->getLimit() == 1 /*just one*/);
            }

            // Default GLE Options
            const bool fsync = false;
            const bool j = false;
            const int w = 1;
            const int wtimeout = 0;

            BSONObj result = conn->getLastErrorDetailed( NamespaceString( request->getNS() ).db()
                                                             .toString(),
                                                         fsync,
                                                         j,
                                                         w,
                                                         wtimeout );

            SafeWriter::fillLastError( result, error );
        }
        catch ( const DBException& ex ) {
            error->raiseError( ex.getCode(), ex.toString().c_str() );
        }
    }

}
