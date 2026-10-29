// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/cidr_range_list_parameter.h"

#include "mongo/idl/idl_parser.h"
#include "mongo/transport/cidr_range_list_parameter_gen.h"
#include "mongo/util/overloaded_visitor.h"

#include <string_view>

namespace mongo::transport {
using namespace std::literals::string_view_literals;

namespace {

CIDRList parseCIDRRangeListParameters(const BSONObj& obj) {
    IDLParserContext ctx("CIDRRangeListParameters");
    const auto params = CIDRRangeListParameter::parse(obj, ctx);
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
                                  std::string_view name) {

    invariant(bob);

    BSONObjBuilder subBob(bob->subobjStart(name));
    BSONArrayBuilder subArray(subBob.subarrayStart("ranges"sv));

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
