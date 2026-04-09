/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/find_command_idl_utils.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/modules.h"

namespace mongo {

class MONGO_MOD_PUB FindCommandRequest : public FindCommandRequestBase {
public:
    explicit FindCommandRequest(
        NamespaceStringOrUUID nssOrUUID,
        boost::optional<SerializationContext> serializationContext = boost::none)
        : FindCommandRequestBase(std::move(nssOrUUID), std::move(serializationContext)) {}

    const NamespaceStringOrUUID& getNamespaceOrUUID() const {
        if (_overrideNssOrUUID) {
            return _overrideNssOrUUID.value();
        }

        return FindCommandRequestBase::getNamespaceOrUUID();
    }

    void setNss(const NamespaceString& nss) {
        _overrideNssOrUUID = NamespaceStringOrUUID{nss};
    }

    static FindCommandRequest parse(const BSONObj& bsonObject,
                                    const IDLParserContext& ctxt,
                                    DeserializationContext* dctx = nullptr) {
        NamespaceString localNS;
        FindCommandRequest object(localNS);
        object.parseProtected(bsonObject, ctxt, dctx);
        return object;
    }

    // The TypedCommand framework requires a parse(OpMsgRequest, ...) method visible on
    // the Request type. The find command IDL defines cpp_name as FindCommandRequestBase,
    // and this class extends it to add custom namespace override behavior. We must
    // explicitly provide this overload for two reasons: (1) the BSONObj parse() above
    // hides all base class parse() overloads due to C++ name hiding, and (2) the base
    // class overloads return FindCommandRequestBase, not FindCommandRequest.
    static FindCommandRequest parse(const OpMsgRequest& request,
                                    const IDLParserContext& ctxt,
                                    DeserializationContext* dctx = nullptr) {
        NamespaceString localNS;
        FindCommandRequest object(localNS);
        object.parseProtected(request, ctxt, dctx);
        return object;
    }

private:
    // This value is never serialized, instead we will serialize out the NamespaceStringOrUUID we
    // parsed when building the FindCommandRequest.
    boost::optional<NamespaceStringOrUUID> _overrideNssOrUUID;
};

}  // namespace mongo
