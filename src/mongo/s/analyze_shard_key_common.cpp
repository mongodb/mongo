// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/analyze_shard_key_common.h"

#include "mongo/db/read_write_concern_provenance_base_gen.h"
#include "mongo/db/repl/read_concern_args.h"

#include <boost/none.hpp>

namespace mongo {
namespace analyze_shard_key {

Status validateNamespace(const NamespaceString& nss) {
    if (!nss.isValid()) {
        return Status(ErrorCodes::InvalidNamespace, str::stream() << "The namespace is invalid");
    }
    if (nss.isOnInternalDb()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot run against an internal collection");
    }
    if (nss.isSystem()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot run against a system collection");
    }
    if (nss.isFLE2StateCollection()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot run against an internal collection");
    }
    return Status::OK();
}

repl::ReadConcernArgs extractReadConcern(OperationContext* opCtx) {
    auto rc = repl::ReadConcernArgs::get(opCtx).toReadConcernIdl();
    rc.setProvenance(boost::none);
    return repl::ReadConcernArgs::fromIDLThrows(std::move(rc));
}

double round(double val, int n) {
    const double multiplier = std::pow(10.0, n);
    return std::ceil(val * multiplier) / multiplier;
}

double calculatePercentage(double part, double whole) {
    invariant(part >= 0);
    invariant(whole > 0);
    invariant(part <= whole);
    return round(part / whole * 100, kMaxNumDecimalPlaces);
}

}  // namespace analyze_shard_key
}  // namespace mongo
