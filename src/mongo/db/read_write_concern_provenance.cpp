// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/read_write_concern_provenance.h"

#include "mongo/util/assert_util.h"

#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

void ReadWriteConcernProvenance::setSource(boost::optional<Source> source) & {
    invariant(!hasSource() || getSource() == source,
              str::stream() << "attempting to re-set provenance, from "
                            << sourceToString(getSource()) << " to " << sourceToString(source));
    ReadWriteConcernProvenanceBase::setSource(std::move(source));
}

ReadWriteConcernProvenance ReadWriteConcernProvenance::parse(const IDLParserContext& ctxt,
                                                             const BSONObj& bsonObject) {
    return ReadWriteConcernProvenance(ReadWriteConcernProvenanceBase::parse(bsonObject, ctxt));
}

std::string_view ReadWriteConcernProvenance::sourceToString(boost::optional<Source> source) {
    return source ? idl::serialize(*source) : "(unset)";
}

}  // namespace mongo
