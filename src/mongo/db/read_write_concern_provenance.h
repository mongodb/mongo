// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/read_write_concern_provenance_base_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
using namespace std::literals::string_view_literals;

/**
 * This class represents the "provenance" (ie. original source) of a read or write concern.
 *
 * This information is serialized as a 'provenance' field inside readConcern/writeConcern objects,
 * which is parsed, stored, and forwarded, but has no other effect on the meaning of the read or
 * write concern (RWC).  This means it is possible to see (in logs, currentOp, profiling, etc) where
 * a given RWC has come from.  It also makes it possible for the server to adjust its behavior based
 * on the origin of the RWC (even though the semantics of the RWC itself have not changed), for
 * example, by having special metrics that only apply to RWC from certain sources.
 *
 * Possible sources are:
 *
 *   - unset (ie. the 'provenance' field was absent):
 *     Implicitly means "clientSupplied" (so drivers do not need to send a 'provenance' field).
 *
 *   - "clientSupplied":
 *     The RWC was originally explicitly passed to a mongos or plain replica set member by the
 *     driver.
 *
 *   - "implicitDefault":
 *     The client did not supply an explicit RWC, and there was no RWC default, so this RWC
 *     represents the corresponding implicit server default, ie. read concern of "local" (except
 *     "available" on sharded secondaries) and {w: 1, wtimeout: 0}.
 *
 *   - "customDefault":
 *     Represents an admin-defined cluster-wide RWC default that was applied to the operation
 *     because no explicit RWC was provided, and there was a custom RWC default defined at the time.
 *
 *   - "getLastErrorDefaults":
 *     Only applicable to write concern, and indicates that it originated from the (deprecated)
 *     'settings.getLastErrorDefaults' field of the replica set configuration.
 *
 *   - "internalWriteDefault":
 *     Only applicable to write concern on internal writes, and indicates that the internal client
 *     did not supply an explicit write concern, so this write concern represents the corresponding
 *     server default for internal writes, ie. {w: 1, wtimeout: 0}.
 *
 * A ReadWriteConcernProvenance object may only have a single Source value throughout its lifetime;
 * once the Source has been set, attempting to change it will trigger an invariant.  This ensures
 * the integrity of the provenance value as the operation makes its way through the server (and
 * through the overall cluster).
 */
class ReadWriteConcernProvenance : public ReadWriteConcernProvenanceBase {
public:
    using Source = ReadWriteConcernProvenanceSourceEnum;

    static constexpr std::string_view kClientSupplied = "clientSupplied"sv;
    static constexpr std::string_view kImplicitDefault = "implicitDefault"sv;
    static constexpr std::string_view kCustomDefault = "customDefault"sv;
    static constexpr std::string_view kGetLastErrorDefaults = "getLastErrorDefaults"sv;
    static constexpr std::string_view kInternalWriteDefault = "internalWriteDefault"sv;

    ReadWriteConcernProvenance() = default;

    ReadWriteConcernProvenance(Source source) {
        setSource(source);
    }

    ReadWriteConcernProvenance(ReadWriteConcernProvenanceBase&& base)
        : ReadWriteConcernProvenanceBase(base) {}

    /**
     * Returns true if this provenance has been set to an actual source, or false if it is unset.
     */
    bool hasSource() const {
        return static_cast<bool>(getSource());
    }

    /**
     * Returns true if the RWC has been supplied by the client, ie. if this provenance's source is
     * either unset (the client specified RWC but without provenance) or explicitly the
     * "clientSupplied" source.
     */
    bool isClientSupplied() const {
        return !hasSource() || *getSource() == Source::clientSupplied;
    }

    /**
     * Returns true if the RWC was an implicit default.
     */
    bool isImplicitDefault() const {
        return hasSource() && *getSource() == Source::implicitDefault;
    }

    /**
     * Returns true if the RWC was a custom default.
     */
    bool isCustomDefault() const {
        return hasSource() && *getSource() == Source::customDefault;
    }

    /**
     * Sets the source of this provenance.  In order to prevent accidental clobbering of provenance
     * with incorrect values, a source cannot change during the provenance's lifetime, except for
     * the initial transition from kUnset to some other Source value.
     *
     * Apart from the initial transition from kUnset to some other Source value, the source of a
     * provenance must not be changed during its lifetime, and this function enforces that property
     * with an invariant.  This is to prevent provenances from being accidentally clobbered with
     * incorrect values.
     */
    void setSource(boost::optional<Source> source) &;

    /**
     * Creates a provenance with source according to the given object's 'provenance' field.
     */
    static ReadWriteConcernProvenance parse(const IDLParserContext& ctxt,
                                            const BSONObj& bsonObject);

    /**
     * Convenience functions.
     */
    static std::string_view sourceToString(boost::optional<Source> source);

    bool operator==(const ReadWriteConcernProvenance& other) const {
        return getSource() == other.getSource();
    }

    bool operator!=(const ReadWriteConcernProvenance& other) const {
        return !operator==(other);
    }
};

}  // namespace mongo
