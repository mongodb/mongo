/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/auth/auth_index_d.h"

#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/structure/collection.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace authindex {

namespace {
    BSONObj v0SystemUsersKeyPattern;
    BSONObj v1SystemUsersKeyPattern;
    BSONObj v2SystemUsersKeyPattern;
    BSONObj v2SystemRolesKeyPattern;
    std::string v2SystemUsersIndexName;
    std::string v2SystemRolesIndexName;

    MONGO_INITIALIZER(AuthIndexKeyPatterns)(InitializerContext*) {
        v0SystemUsersKeyPattern = BSON(AuthorizationManager::V1_USER_NAME_FIELD_NAME << 1);
        v1SystemUsersKeyPattern = BSON(AuthorizationManager::V1_USER_NAME_FIELD_NAME << 1 <<
                                       AuthorizationManager::V1_USER_SOURCE_FIELD_NAME << 1);
        v2SystemUsersKeyPattern = BSON(AuthorizationManager::USER_NAME_FIELD_NAME << 1 <<
                                       AuthorizationManager::USER_SOURCE_FIELD_NAME << 1);
        v2SystemRolesKeyPattern = BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME << 1 <<
                                       AuthorizationManager::ROLE_SOURCE_FIELD_NAME << 1);
        v2SystemUsersIndexName = std::string(
                str::stream() <<
                        AuthorizationManager::USER_NAME_FIELD_NAME << "_1_" <<
                        AuthorizationManager::USER_SOURCE_FIELD_NAME << "_1");
        v2SystemRolesIndexName = std::string(
                str::stream() <<
                        AuthorizationManager::ROLE_NAME_FIELD_NAME << "_1_" <<
                        AuthorizationManager::ROLE_SOURCE_FIELD_NAME << "_1");
        return Status::OK();
    }

    void configureSystemUsersIndexes(const StringData& dbname) {
        if (dbname != "admin") return;
        if (getGlobalAuthorizationManager()->getAuthorizationVersion() !=
            AuthorizationManager::schemaVersion26Final) {

            return;
        }
        std::string systemUsers = dbname.toString() + ".system.users";
        Client::WriteContext wctx(systemUsers);

        NamespaceString systemUsersNS( systemUsers );
        createSystemIndexes(systemUsersNS);

        Collection* collection = wctx.ctx().db()->getCollection( systemUsers );
        if (collection == NULL)
            return;

        NamespaceDetails::IndexIterator indexIter = collection->details()->ii();
        std::vector<std::string> namedIndexesToDrop;

        while (indexIter.more()) {
            IndexDetails& idetails = indexIter.next();
            if (idetails.keyPattern() == v0SystemUsersKeyPattern ||
                    idetails.keyPattern() == v1SystemUsersKeyPattern)
                namedIndexesToDrop.push_back(idetails.indexName());
        }

        for (size_t i = 0; i < namedIndexesToDrop.size(); ++i) {
            int idxNo = collection->details()->findIndexByName( namedIndexesToDrop[i] );
            verify( idxNo >= 0 );

            Status status = collection->getIndexCatalog()->dropIndex( idxNo );
            if ( status.isOK() ) {
                log() << "Dropped index " << namedIndexesToDrop[i] << " from " << systemUsers <<
                    " because it is incompatible with v2 form privilege documents." << endl;
            }
            else {
                // Only reason should be orphaned index, which dropIndexes logged.
                LOG(2) << "configureSystemUsersIndexes couldn't drop index: "
                       << namedIndexesToDrop[i]
                       << " but should not matter";
            }
        }
    }

    void configureSystemRolesIndexes(const StringData& dbname) {
        std::string systemRoles = dbname.toString() + ".system.roles";
        Client::WriteContext wctx(systemRoles);

        NamespaceString systemRolesNS( systemRoles );
        createSystemIndexes(systemRolesNS);
    }
}  // namespace

    void configureSystemIndexes(const StringData& dbname) {
        configureSystemUsersIndexes(dbname);
        configureSystemRolesIndexes(dbname);
    }

    void createSystemIndexes(const NamespaceString& ns) {
        if (ns == AuthorizationManager::usersCollectionNamespace &&
            (getGlobalAuthorizationManager()->getAuthorizationVersion() ==
             AuthorizationManager::schemaVersion26Final)) {
            try {
                Helpers::ensureIndex(ns.ns().c_str(),
                                     v2SystemUsersKeyPattern,
                                     true,  // unique
                                     v2SystemUsersIndexName.c_str());
            } catch (const DBException& e) {
                if (e.getCode() == ASSERT_ID_DUPKEY) {
                    log() << "Duplicate key exception while trying to build unique index on " <<
                            ns << ".  This is likely due to problems during the upgrade process " <<
                            endl;
                }
                throw;
            }
        } else if (ns == AuthorizationManager::rolesCollectionNamespace) {
            try {
                Helpers::ensureIndex(ns.ns().c_str(),
                                     v2SystemRolesKeyPattern,
                                     true,  // unique
                                     v2SystemRolesIndexName.c_str());
            } catch (const DBException& e) {
                if (e.getCode() == ASSERT_ID_DUPKEY) {
                    log() << "Duplicate key exception while trying to build unique index on " <<
                            ns << "." << endl;
                }
                throw;
            }
        }
    }

}  // namespace authindex
}  // namespace mongo
