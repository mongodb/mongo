// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/util/builder.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <iosfwd>
#include <string>
#include <string_view>

#include <boost/optional.hpp>
#include <fmt/format.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class Status;
template <typename T>
class StatusWith;

/**
 * Validate that a string is either empty or is parseable to a HostAndPort. This is intended for use
 * as an IDL validator callback.
 */
Status validateHostAndPort(std::string_view hostAndPortStr, const boost::optional<TenantId>&);

/**
 * Name of a process on the network.
 *
 * Composed of some name component, followed optionally by a colon and a numeric port.  The name
 * might be an IPv4 or IPv6 address or a relative or fully qualified host name, or an absolute
 * path to a unix socket.
 */
struct HostAndPort {
    /**
     * Parses "text" to produce a HostAndPort.  Returns either that or an error status describing
     * the parse failure.
     */
    static StatusWith<HostAndPort> parse(std::string_view text);

    /**
     * A version of 'parse' that throws a UserException if a parsing error is encountered.
     */
    static HostAndPort parseThrowing(std::string_view text) {
        return uassertStatusOK(parse(text));
    }

    /**
     * Construct an empty/invalid HostAndPort.
     */
    HostAndPort();

    /**
     * Constructs a HostAndPort by parsing "text" of the form hostname[:portnumber]
     * Throws an AssertionException if bad config std::string or bad port #.
     */
    explicit HostAndPort(std::string_view text);

    /**
     * Constructs a HostAndPort with the hostname "h" and port "p".
     *
     * If "p" is -1, port() returns ServerGlobalParams::DefaultDBPort.
     */
    HostAndPort(std::string h, int p);

    /**
     * (Re-)initializes this HostAndPort by parsing "s".  Returns
     * Status::OK on success.  The state of this HostAndPort is unspecified
     * after initialize() returns a non-OK status, though it is safe to
     * assign to it or re-initialize it.
     */
    Status initialize(std::string_view s);

    bool operator<(const HostAndPort& r) const;
    bool operator==(const HostAndPort& r) const;
    bool operator!=(const HostAndPort& r) const {
        return !(*this == r);
    }

    /**
     * Returns true if the hostname looks localhost-y.
     *
     * TODO: Make a more rigorous implementation, perhaps elsewhere in
     * the networking library.
     */
    bool isLocalHost() const;

    /**
     * Returns true if the hostname is an IP matching the default route.
     */
    bool isDefaultRoute() const;

    /**
     * Returns true if the hostname is a Unix domain socket.
     */
    bool isUds() const;

    /**
     * Returns a string representation of "host:port".
     */
    std::string toString() const;

    /**
     * Returns true if this object represents no valid HostAndPort.
     */
    bool empty() const;

    const std::string& host() const {
        return _host;
    }
    int port() const;

    bool hasPort() const {
        return _port >= 0;
    }

    template <typename H>
    friend H AbslHashValue(H h, const HostAndPort& hostAndPort) {
        return H::combine(std::move(h), hostAndPort.host(), hostAndPort.port());
    }

private:
    friend struct fmt::formatter<HostAndPort>;

    struct AppendVisitor {
        virtual void operator()(std::string_view v) = 0;
        virtual void operator()(std::uint16_t v) = 0;
        virtual ~AppendVisitor() = default;
    };

    void _appendToVisitor(AppendVisitor& sink) const;

    template <typename F>
    void _appendToPolymorphicFunc(F f) const;

    template <typename Stream>
    Stream& _appendToStream(Stream& os) const {
        _appendToPolymorphicFunc([&](const auto& v) { os << v; });
        return os;
    }

    friend std::ostream& operator<<(std::ostream& os, const HostAndPort& hp) {
        return hp._appendToStream(os);
    }
    friend StringBuilder& operator<<(StringBuilder& os, const HostAndPort& hp) {
        return hp._appendToStream(os);
    }
    friend StackStringBuilder& operator<<(StackStringBuilder& os, const HostAndPort& hp) {
        return hp._appendToStream(os);
    }

    std::string _host;
    int _port;  // -1 indicates unspecified
};

template <typename F>
void HostAndPort::_appendToPolymorphicFunc(F f) const {
    struct Vis : AppendVisitor {
        explicit Vis(F f) : _f{std::move(f)} {}
        void operator()(std::string_view v) override {
            _f(v);
        }
        void operator()(std::uint16_t v) override {
            _f(v);
        }
        F _f;
    };
    Vis visitor(std::move(f));
    _appendToVisitor(visitor);
}

}  // namespace mongo

namespace fmt {
template <>
struct formatter<mongo::HostAndPort> {
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(const mongo::HostAndPort& hp, FormatContext& ctx) const {
        auto&& it = ctx.out();
        hp._appendToPolymorphicFunc([&](const auto& v) { it = fmt::format_to(it, "{}", v); });
        return it;
    }
};
}  // namespace fmt
