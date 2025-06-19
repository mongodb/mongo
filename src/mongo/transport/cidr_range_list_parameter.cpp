/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/transport/cidr_range_list_parameter.h"

#include "mongo/idl/idl_parser.h"
#include "mongo/transport/cidr_range_list_parameter_gen.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::transport {

namespace {

CIDRList parseCIDRRangeListParameters(const BSONObj& obj) {
    IDLParserContext ctx("CIDRRangeListParameters");
    const auto params = CIDRRangeListParameter::parse(ctx, obj);
    CIDRList output;
    for (const auto& range : params.getRanges()) {
        auto swr = CIDR::parse(range);
        if (!swr.isOK()) {
            output.emplace_back(std::in_place_type<std::string>, range);
        } else {
            output.emplace_back(std::in_place_type<CIDR>, std::move(swr.getValue()));
        }
    }
    return output;
}

}  // namespace

// TODO: SERVER-106468 Move this function to the anonymous namespace.
void appendCIDRRangeListParameter(VersionedValue<CIDRList>& value,
                                  BSONObjBuilder* bob,
                                  StringData name) {

    invariant(bob);

    BSONObjBuilder subBob(bob->subobjStart(name));
    BSONArrayBuilder subArray(subBob.subarrayStart("ranges"_sd));

    auto snapshot = value.makeSnapshot();
    if (!snapshot)
        return;

    for (const auto& range : *snapshot) {
        subArray.append(std::visit(OverloadedVisitor{
                                       [](const CIDR& arg) { return arg.toString(); },
                                       [](const std::string& arg) { return arg; },
                                   },
                                   range));
    }
}

// TODO: SERVER-106468 Move this function to the anonymous namespace.
Status setCIDRRangeListParameter(VersionedValue<CIDRList>& value, BSONObj obj) try {
    auto list = parseCIDRRangeListParameters(obj);
    value.update(std::make_shared<CIDRList>(std::move(list)));
    return Status::OK();
} catch (const DBException& e) {
    return e.toStatus();
}

}  // namespace mongo::transport
