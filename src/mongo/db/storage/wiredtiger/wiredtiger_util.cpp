// wiredtiger_util.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *    Copyright (C) 2014 WiredTiger Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    using std::string;

    Status WiredTigerUtil::exportTableToBSON(WT_SESSION* s,
                                             const std::string& uri, const std::string& config,
                                             BSONObjBuilder* bob) {
        invariant(s);
        invariant(bob);
        WT_CURSOR* c = NULL;
        const char *cursorConfig = config.empty() ? NULL : config.c_str();
        int ret = s->open_cursor(s, uri.c_str(), NULL, cursorConfig, &c);
        if (ret != 0) {
            return Status(ErrorCodes::CursorNotFound, str::stream()
                << "unable to open cursor at URI " << uri
                << ". reason: " << wiredtiger_strerror(ret));
        }
        bob->append("uri", uri);
        invariant(c);
        ON_BLOCK_EXIT(c->close, c);
        const char *desc, *pvalue;
        uint64_t value;
        while (c->next(c) == 0 && c->get_value(c, &desc, &pvalue, &value) == 0) {
            bob->append(desc, pvalue);
        }
        return Status::OK();
    }

}  // namespace mongo
