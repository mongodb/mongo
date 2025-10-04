/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/election_reason_counter_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace repl {

/**
 * Wrapper around the IDL struct ElectionReasonCounterBase that has increment methods.
 */
class ElectionReasonCounter : public ElectionReasonCounterBase {
public:
    using ElectionReasonCounterBase::getCalled;
    using ElectionReasonCounterBase::getSuccessful;
    using ElectionReasonCounterBase::setCalled;
    using ElectionReasonCounterBase::setSuccessful;

    void incrementCalled() {
        setCalled(getCalled() + 1);
    }

    void incrementSuccessful() {
        setSuccessful(getSuccessful() + 1);
    }

    ElectionReasonCounter parse(const IDLParserContext& ctxt, const BSONObj& bsonObject);
};

}  // namespace repl
}  // namespace mongo
