// list_collections.cpp

/**
*    Copyright (C) 2014 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/storage/storage_engine.h"

namespace mongo {

    class CmdListCollections : public Command {
    public:
        virtual bool slaveOk() const { return true; }
        virtual bool slaveOverrideOk() const { return true; }
        virtual bool adminOnly() const { return false; }
        virtual bool isWriteCommandForConfigServer() const { return false; }

        virtual void help( stringstream& help ) const { help << "list collections for this db"; }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::listCollections);
            out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
        }

        CmdListCollections() : Command( "listCollections", true ) {}

        bool run(OperationContext* txn,
                 const string& dbname,
                 BSONObj& jsobj,
                 int,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool /*fromRepl*/) {

            Client::ReadContext ctx( txn, dbname );
            const Database* d = ctx.ctx().db();
            const DatabaseCatalogEntry* dbEntry = d->getDatabaseCatalogEntry();

            list<string> names;
            dbEntry->getCollectionNamespaces( &names );

            BSONArrayBuilder arr;

            for ( list<string>::const_iterator i = names.begin(); i != names.end(); ++i ) {
                string ns = *i;

                BSONObjBuilder b;
                b.append( "name", nsToCollectionSubstring( ns ) );

                CollectionOptions options =
                    dbEntry->getCollectionCatalogEntry( txn, ns )->getCollectionOptions();
                b.append( "options", options.toBSON() );

                arr.append( b.obj() );
            }

            result.append( "collections", arr.arr() );

            return true;
        }

    } cmdListCollections;

}
