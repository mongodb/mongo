// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/decorable.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
using namespace std::literals::string_view_literals;

/**
 * Decorates operation context with methods to retrieve apiVersion, apiStrict, and
 * apiDeprecationErrors.
 */
class APIParameters {

public:
    static constexpr std::string_view kAPIVersionFieldName = "apiVersion"sv;
    static constexpr std::string_view kAPIStrictFieldName = "apiStrict"sv;
    static constexpr std::string_view kAPIDeprecationErrorsFieldName = "apiDeprecationErrors"sv;

    static const OperationContext::Decoration<APIParameters> get;
    static APIParameters fromClient(const APIParametersFromClient& apiParamsFromClient);
    static APIParameters fromBSON(const BSONObj& cmdObj);

    // For use with unordered_map.
    struct Hash {
        std::size_t operator()(const APIParameters& params) const;
    };

    void appendInfo(BSONObjBuilder* builder) const;

    /**
     * Set the API parameters on an IDL-defined command or GenericArguments struct.
     */
    template <typename CommandType>
    void setInfo(CommandType& request) const {
        request.setApiStrict(getAPIStrict());
        if (auto& apiVersion = getAPIVersion()) {
            request.setApiVersion(std::string_view(*apiVersion));
        } else {
            request.setApiVersion(boost::none);
        }
        request.setApiDeprecationErrors(getAPIDeprecationErrors());
    }

    BSONObj toBSON() const;

    const boost::optional<std::string>& getAPIVersion() const {
        return _apiVersion;
    }

    void setAPIVersion(std::string_view apiVersion) {
        _apiVersion = std::string{apiVersion};
    }

    const boost::optional<bool>& getAPIStrict() const {
        return _apiStrict;
    }

    void setAPIStrict(bool apiStrict) {
        _apiStrict = apiStrict;
    }

    const boost::optional<bool>& getAPIDeprecationErrors() const {
        return _apiDeprecationErrors;
    }

    void setAPIDeprecationErrors(bool apiDeprecationErrors) {
        _apiDeprecationErrors = apiDeprecationErrors;
    }

    bool getParamsPassed() const {
        return _apiVersion || _apiStrict || _apiDeprecationErrors;
    }

private:
    boost::optional<std::string> _apiVersion;
    boost::optional<bool> _apiStrict;
    boost::optional<bool> _apiDeprecationErrors;
};

inline bool operator==(const APIParameters& lhs, const APIParameters& rhs) {
    return lhs.getAPIVersion() == rhs.getAPIVersion() && lhs.getAPIStrict() == rhs.getAPIStrict() &&
        lhs.getAPIDeprecationErrors() == rhs.getAPIDeprecationErrors();
}

inline bool operator!=(const APIParameters& lhs, const APIParameters& rhs) {
    return !(lhs == rhs);
}

/**
 * Temporarily remove the user's API parameters from an OperationContext.
 */
class IgnoreAPIParametersBlock {
public:
    IgnoreAPIParametersBlock() = delete;
    IgnoreAPIParametersBlock(const IgnoreAPIParametersBlock&) = delete;
    IgnoreAPIParametersBlock& operator=(const IgnoreAPIParametersBlock&) = delete;

    explicit IgnoreAPIParametersBlock(OperationContext* opCtx) : _opCtx(opCtx) {
        _apiParams = APIParameters::get(_opCtx);
        APIParameters::get(_opCtx) = APIParameters();
    }

    void release() {
        if (_released) {
            return;
        }

        APIParameters::get(_opCtx) = _apiParams;
        _released = true;
    }

    ~IgnoreAPIParametersBlock() {
        release();
    }

private:
    OperationContext* _opCtx;
    APIParameters _apiParams;
    bool _released = false;
};

}  // namespace mongo
