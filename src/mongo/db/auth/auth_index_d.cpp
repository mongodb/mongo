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
*/

#include "mongo/db/auth/auth_index_d.h"

#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_update.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_details.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace authindex {

namespace {
    BSONObj v0SystemUsersKeyPattern;
    BSONObj v1SystemUsersKeyPattern;
    BSONObj v2SystemUsersKeyPattern;
    std::string v2SystemUsersIndexName;

    MONGO_INITIALIZER(AuthIndexKeyPatterns)(InitializerContext*) {
        v0SystemUsersKeyPattern = BSON(AuthorizationManager::V1_USER_NAME_FIELD_NAME << 1);
        v1SystemUsersKeyPattern = BSON(AuthorizationManager::V1_USER_NAME_FIELD_NAME << 1 <<
                                       AuthorizationManager::V1_USER_SOURCE_FIELD_NAME << 1);
        v2SystemUsersKeyPattern = BSON(AuthorizationManager::USER_NAME_FIELD_NAME << 1 <<
                                       AuthorizationManager::USER_SOURCE_FIELD_NAME << 1);
        v2SystemUsersIndexName = std::string(
                str::stream() <<
                        AuthorizationManager::USER_NAME_FIELD_NAME << "_1_" <<
                        AuthorizationManager::USER_SOURCE_FIELD_NAME << "_1");
        return Status::OK();
    }

    void configureSystemUsersIndexes(const StringData& dbname) {
        std::string systemUsers = dbname.toString() + ".system.users";
        Client::WriteContext wctx(systemUsers);

        NamespaceString systemUsersNS( systemUsers );
        createSystemIndexes(systemUsersNS);

        NamespaceDetails* nsd = nsdetails(systemUsers);
        if (nsd == NULL)
            return;

        NamespaceDetails::IndexIterator indexIter = nsd->ii();
        std::vector<std::string> namedIndexesToDrop;

        while (indexIter.more()) {
            IndexDetails& idetails = indexIter.next();
            if (idetails.keyPattern() == v0SystemUsersKeyPattern ||
                    idetails.keyPattern() == v1SystemUsersKeyPattern)
                namedIndexesToDrop.push_back(idetails.indexName());
        }
        for (size_t i = 0; i < namedIndexesToDrop.size(); ++i) {
            std::string errmsg;
            BSONObjBuilder infoBuilder;

            if (dropIndexes(nsd,
                            systemUsers.c_str(),
                            namedIndexesToDrop[i].c_str(),
                            errmsg,
                            infoBuilder,
                            false)) {
                log() << "Dropped index " << namedIndexesToDrop[i] << " from " << systemUsers <<
                    " because it is incompatible with v2 form privilege documents." << endl;
            }
            else {
                // Only reason should be orphaned index, which dropIndexes logged.
            }
        }
    }
}  // namespace

    void configureSystemIndexes(const StringData& dbname) {
        configureSystemUsersIndexes(dbname);
    }

    void createSystemIndexes(const NamespaceString& ns) {
        if (ns.coll() == "system.users") {
            try {
                Helpers::ensureIndex(ns.ns().c_str(),
                                     v2SystemUsersKeyPattern,
                                     true,  // unique
                                     v2SystemUsersIndexName.c_str());
            } catch (const DBException& e) {
                if (e.getCode() == ASSERT_ID_DUPKEY) {
                    log() << "Duplicate key exception while trying to build unique index on " <<
                            ns << ".  This is likely due to upgrade process shenanigans" << endl;
                }
                throw;
            }
        }
    }

}  // namespace authindex
}  // namespace mongo
