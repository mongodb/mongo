// wiredtiger_util.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <limits>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    using std::string;

    int64_t WiredTigerUtil::getIdentSize(WT_SESSION* s,
                                         const std::string& uri ) {
        BSONObjBuilder b;
        Status status = WiredTigerUtil::exportTableToBSON(s,
                                                          "statistics:" + uri,
                                                          "statistics=(fast)",
                                                          &b);
        if ( !status.isOK() ) {
            if ( status.code() == ErrorCodes::CursorNotFound ) {
                // ident gone, so its 0
                return 0;
            }
            uassertStatusOK( status );
        }

        BSONObj obj = b.obj();
        BSONObj sub = obj["block-manager"].Obj();
        BSONElement e = sub["file size in bytes"];
        invariant( e.type() );

        if ( e.isNumber() )
            return e.safeNumberLong();

        return strtoull( e.valuestrsafe(), NULL, 10 );
    }


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

        std::map<string,BSONObjBuilder*> subs;
        const char *desc, *pvalue;
        uint64_t value;
        while (c->next(c) == 0 && c->get_value(c, &desc, &pvalue, &value) == 0) {
            StringData key( desc );

            StringData prefix;
            StringData suffix;

            size_t idx = key.find( ':' );
            if ( idx != string::npos ) {
                prefix = key.substr( 0, idx );
                suffix = key.substr( idx + 1 );
            }
            else {
                idx = key.find( ' ' );
            }

            if ( idx != string::npos ) {
                prefix = key.substr( 0, idx );
                suffix = key.substr( idx + 1 );
            }
            else {
                prefix = key;
                suffix = "num";
            }

            // Convert unsigned 64-bit integral value of statistic to BSON-friendly long long.
            // If there is an overflow, set statistic value to max(long long).
            const long long maxLL = std::numeric_limits<long long>::max();
            long long v =  value > static_cast<uint64_t>(maxLL) ?
                maxLL : static_cast<long long>(value);

            if ( prefix.size() == 0 ) {
                bob->appendNumber(desc, v);
            }
            else {
                BSONObjBuilder*& sub = subs[prefix.toString()];
                if ( !sub )
                    sub = new BSONObjBuilder();
                sub->appendNumber(mongoutils::str::ltrim(suffix.toString()), v);
            }
        }

        for ( std::map<string,BSONObjBuilder*>::const_iterator it = subs.begin();
              it != subs.end(); ++it ) {
            const std::string& s = it->first;
            bob->append( s, it->second->obj() );
            delete it->second;
        }
        return Status::OK();
    }

}  // namespace mongo
