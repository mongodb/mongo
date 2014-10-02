/**
 *    Copyright (C) 2012-2014 MongoDB Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/string_map.h"

namespace mongo {

    /**
     * Registry of opened databases.
     */
    class DatabaseHolder {
        typedef StringMap<Database*> DBs;
        // todo: we want something faster than this if called a lot:
        mutable SimpleMutex _m;
        DBs _dbs;
    public:
        DatabaseHolder() : _m("dbholder") { }

        Database* get(OperationContext* txn,
                      const StringData& ns) const;

        Database* getOrCreate(OperationContext* txn,
                              const StringData& ns,
                              bool& justCreated);


        void close(OperationContext* txn, const StringData& ns);

        /** @param force - force close even if something underway - use at shutdown */
        bool closeAll(OperationContext* txn,
                      BSONObjBuilder& result,
                      bool force);

        /**
         * need some lock
         */
        void getAllShortNames( std::set<std::string>& all ) const {
            SimpleMutex::scoped_lock lk(_m);
            for( DBs::const_iterator j=_dbs.begin(); j!=_dbs.end(); ++j ) {
                all.insert( j->first );
            }
        }

    private:
        static StringData _todb( const StringData& ns ) {
            StringData d = __todb( ns );
            uassert(13280, "invalid db name: " + ns.toString(), NamespaceString::validDBName(d));
            return d;
        }
        static StringData __todb( const StringData& ns ) {
            size_t i = ns.find( '.' );
            if ( i == std::string::npos ) {
                uassert( 13074 , "db name can't be empty" , ns.size() );
                return ns;
            }
            uassert( 13075 , "db name can't be empty" , i > 0 );
            return ns.substr( 0 , i );
        }
    };

    DatabaseHolder& dbHolder();
}
