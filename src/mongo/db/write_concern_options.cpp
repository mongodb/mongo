/*    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/write_concern_options.h"

#include "mongo/bson/bson_field.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/field_parser.h"

namespace mongo {

    using std::string;

    const BSONObj WriteConcernOptions::Default = BSONObj();
    const BSONObj WriteConcernOptions::Acknowledged(BSON("w" << W_NORMAL));
    const BSONObj WriteConcernOptions::Unacknowledged(BSON("w" << W_NONE));

    static const BSONField<bool> mongosSecondaryThrottleField("_secondaryThrottle", true);
    static const BSONField<bool> secondaryThrottleField("secondaryThrottle", true);
    static const BSONField<BSONObj> writeConcernField("writeConcern");

    WriteConcernOptions::WriteConcernOptions(int numNodes,
                                             SyncMode sync,
                                             int timeout):
                            syncMode(sync),
                            wNumNodes(numNodes),
                            wTimeout(timeout) {
    }

    WriteConcernOptions::WriteConcernOptions(const std::string& mode,
                                             SyncMode sync,
                                             int timeout):
                            syncMode(sync),
                            wNumNodes(0),
                            wMode(mode),
                            wTimeout(timeout) {
    }

    Status WriteConcernOptions::parse( const BSONObj& obj ) {
        if ( obj.isEmpty() ) {
            return Status( ErrorCodes::FailedToParse, "write concern object cannot be empty" );
        }

        BSONElement jEl = obj["j"];
        if ( !jEl.eoo() && !jEl.isNumber() && jEl.type() != Bool ) {
            return Status( ErrorCodes::FailedToParse, "j must be numeric or a boolean value" );
        }

        const bool j = jEl.trueValue();

        BSONElement fsyncEl = obj["fsync"];
        if ( !fsyncEl.eoo() && !fsyncEl.isNumber() && fsyncEl.type() != Bool ) {
            return Status( ErrorCodes::FailedToParse, "fsync must be numeric or a boolean value" );
        }

        const bool fsync = fsyncEl.trueValue();

        if ( j && fsync )
            return Status( ErrorCodes::FailedToParse,
                           "fsync and j options cannot be used together" );

        if ( j ) {
            syncMode = JOURNAL;
        }
        if ( fsync ) {
            syncMode = FSYNC;
        }

        BSONElement e = obj["w"];
        if ( e.isNumber() ) {
            wNumNodes = e.numberInt();
        }
        else if ( e.type() == String ) {
            wMode = e.valuestrsafe();
        }
        else if ( e.eoo() ||
                  e.type() == jstNULL ||
                  e.type() == Undefined ) {
            wNumNodes = 1;
        }
        else {
            return Status( ErrorCodes::FailedToParse, "w has to be a number or a string" );
        }

        wTimeout = obj["wtimeout"].numberInt();

        return Status::OK();
    }

    Status WriteConcernOptions::parseSecondaryThrottle(const BSONObj& doc,
                                                       BSONObj* rawWriteConcernObj) {
        string errMsg;
        bool isSecondaryThrottle;
        FieldParser::FieldState fieldState = FieldParser::extract(doc,
                                                                  secondaryThrottleField,
                                                                  &isSecondaryThrottle,
                                                                  &errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) {
            return Status(ErrorCodes::FailedToParse, errMsg);
        }

        if (fieldState != FieldParser::FIELD_SET) {
            fieldState = FieldParser::extract(doc,
                                              mongosSecondaryThrottleField,
                                              &isSecondaryThrottle,
                                              &errMsg);

            if (fieldState == FieldParser::FIELD_INVALID) {
                return Status(ErrorCodes::FailedToParse, errMsg);
            }
        }

        BSONObj dummyBSON;
        if (!rawWriteConcernObj) {
            rawWriteConcernObj = &dummyBSON;
        }

        fieldState = FieldParser::extract(doc,
                                          writeConcernField,
                                          rawWriteConcernObj,
                                          &errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) {
            return Status(ErrorCodes::FailedToParse, errMsg);
        }

        if (!isSecondaryThrottle) {
            if (!rawWriteConcernObj->isEmpty()) {
                return Status(ErrorCodes::UnsupportedFormat,
                              "Cannot have write concern when secondary throttle is false");
            }

            wNumNodes = 1;
            return Status::OK();
        }

        if (rawWriteConcernObj->isEmpty()) {
            return Status(ErrorCodes::WriteConcernNotDefined,
                          "Secondary throttle is on, but write concern is not specified");
        }

        return parse(*rawWriteConcernObj);
    }

    BSONObj WriteConcernOptions::toBSON() const {
        BSONObjBuilder builder;

        if (wMode.empty()) {
            builder.append("w", wNumNodes);
        }
        else {
            builder.append("w", wMode);
        }

        if (syncMode == FSYNC) {
            builder.append("fsync", true);
        }
        else if (syncMode == JOURNAL) {
            builder.append("j", true);
        }

        builder.append("wtimeout", wTimeout);

        return builder.obj();
    }

    bool WriteConcernOptions::shouldWaitForOtherNodes() const {
        return !wMode.empty() || wNumNodes > 1;
    }
}
