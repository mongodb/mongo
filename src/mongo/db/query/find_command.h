// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/find_command_idl_utils.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_PUBLIC]] FindCommandRequest : public FindCommandRequestBase {
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
