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

#include <algorithm>
#include <cctype>
#include <iostream>
#include <iterator>
#include <sstream>
#include <tuple>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace dns {

/**
 * A `dns::HostName` represents a DNS Hostname in a form which is suitable for programatic
 * manipulation.
 *
 * Oftentimes it is inappropriate to operate on a domain name (DNS Hostname) as a string.  Besides
 * the obvious limitations and cognitive overhead implied by string processing, there are
 * fundamental semantic conventions in the format of a DNS Hostname which need to be handled
 * appropriately.  This type, `dns::HostName` represents a DNS Hostname in native C++ types and
 * provides a set of salient member functions which exhibit the expected behavior and semantics of a
 * DNS Hostname.  It is encouraged to rewrite code which handles domain names from raw string form
 * into code using this type instead.  A type which represents and obeys correct DNS Hostname
 * semantics will help prevent bugs in name handling and resolution.
 */
class HostName {
public:
    /**
     * A `dns::HostName` can be either Fully Qualified (FQDN) or a relative name.
     *
     * Some member functions of `dns::HostName` may behave differently depending upon whether a
     * Hostname is fully qualified or not.
     */
    enum Qualification : bool { kRelativeName = false, kFullyQualified = true };

public:
    /**
     * Constructs a parsed DNS Hostname representation from the specified string.
     *
     * A DNS name can be fully qualified (ending in a '.') or unqualified (not ending in a '.').
     * When the specified `dnsName` string ends in a terminating dot (`'.'`) character, the
     * constructed `dns::HostName` object will have the `Qualification::kFullyQualified` state,
     * otherwise it will have the `Qualification::kRelativeName` state.  This constructor will parse
     * the specified name, separating it on the dot (`'.'`) tokens for simpler programatic
     * processing.
     *
     * THROWS: `DBException` with `ErrorCodes::DNSRecordTypeMismatch` as the status value if the
     * name is ill formatted.
     */
    explicit HostName(StringData dnsName) {
        if (dnsName.empty())
            uasserted(ErrorCodes::DNSRecordTypeMismatch,
                      "A Domain Name cannot have zero characters");

        if (dnsName[0] == '.')
            uasserted(ErrorCodes::DNSRecordTypeMismatch,
                      "A Domain Name cannot start with a '.' character.");

        enum ParserState { kFirstLetter, kNonPeriod, kPeriod };
        ParserState parserState = kFirstLetter;

        std::string name;
        int idx = -1;
        for (const char& ch : dnsName) {
            ++idx;
            if (ch == '.') {
                if (parserState == kPeriod) {
                    uasserted(ErrorCodes::DNSRecordTypeMismatch,
                              "A Domain Name cannot have two adjacent '.' characters");
                }
                parserState = kPeriod;
                this->_nameComponents.push_back(std::move(name));
                name.clear();
                continue;
            }
            if (parserState == kPeriod) {
                parserState = kFirstLetter;
            }


            invariant(ch != '.');

            // We permit dashes and numbers.  We also permit underscores for use with SRV records
            // and such.
            if (!(ch == '-' || std::isalnum(ch) || (ch == '_' && parserState == kFirstLetter))) {
                uasserted(ErrorCodes::DNSRecordTypeMismatch,
                          "A Domain Name cannot have tokens other than dash or alphanumerics.");
            }
            // All domain names are represented in lower-case letters, because DNS is case
            // insensitive.
            name.push_back(std::tolower(ch));
            if (parserState == kFirstLetter) {
                parserState = kNonPeriod;
            }
        }

        if (parserState == kPeriod)
            _fullyQualified = kFullyQualified;
        else {
            _fullyQualified = kRelativeName;
            _nameComponents.push_back(std::move(name));
        }

        if (_nameComponents.empty())
            uasserted(ErrorCodes::DNSRecordTypeMismatch,
                      "A Domain Name cannot have zero name elements");

        if (isEquivalentToIPv4DottedDecimal()) {
            uasserted(ErrorCodes::DNSRecordTypeMismatch,
                      "A Domain Name cannot be equivalent in form to an IPv4 address");
        }

        // Reverse all the names, once we've parsed them all in.
        std::reverse(begin(_nameComponents), end(_nameComponents));
    }

    /**
     * Returns whether this DNS Hostname has been fully qualified.
     *
     * A DNS Hostname is considered fully qualified, if the canonical specification of its name
     * includes a trailing `'.'`.  Fully Qualified Domain Names (FQDNs) are always resolved against
     * the root name servers and indicate absolute names.  Unqualified names are looked up against
     * DNS configuration specific prefixes, recursively, until a match is found, which may not be
     * the corresponding FQDN.
     *
     * RETURNS: True if this hostname is an FQDN and false otherwise.
     */
    bool isFQDN() const {
        return _fullyQualified;
    }

    /**
     * Changes the qualification of this `dns::HostName` to the specified `qualification`.
     *
     * An unqualified domain hostname may exist as an artifact of other protocols wherein the actual
     * qualification of that name is implied to be complete.  When operating on such names in
     * `dns::HostName` form, it may be necessary to alter the qualification after the fact.
     *
     * POST: The qualification of `*this` will be changed to the qualification specified by
     * `qualification`.
     */
    void forceQualification(const Qualification qualification = kFullyQualified) {
        _fullyQualified = qualification;
    }

    /**
     * Returns the complete canonical name for this `dns::HostName`.
     *
     * The canonical form for a DNS Hostname is the complete dotted DNS path, including a trailing
     * dot (if the domain in question is fully qualified).  A DNS Hostname which is fully qualified
     * (ending in a trailing dot) will not compare equal (in string form) to a DNS Hostname which
     * has not been fully qualified.  This representation may be unsuitable for some use cases which
     * involve relaxed qualification indications.
     *
     * RETURNS: A `std::string` which represents this DNS Hostname in complete canonical form.
     */
    std::string canonicalName() const {
        StringBuilder sb;
        stream(sb);
        return sb.str();
    }

    /**
     * Returns the complete name for this `dns::HostName` in a form suitable for use with SSL
     * certificate names.
     *
     * For myriad reasons, SSL certificates do not specify the fully qualified name of any host.
     * When using `dns::HostName` objects in SSL aware code, it may be necessary to get an
     * unqualified string form for use in certificate name comparisons.
     *
     * RETURNS: A `std::string` which represents this Hostname without a trailing dot (`'.'`).
     */
    std::string noncanonicalName() const {
        StringBuilder sb;
        streamUnqualified(sb);
        return sb.str();
    }

    /**
     * Returns the number of subdomain components in this `dns::HostName`.
     *
     * A DNS Hostname is composed of at least one, and sometimes more, subdomains.  This function
     * indicates how many subdomains this `dns::HostName` specifier has.  Each subdomain is
     * separated by a single `'.'` character.
     *
     * RETURNS: The number of components in `this->nameComponents()`
     */
    std::size_t depth() const {
        return this->_nameComponents.size();
    }

    /**
     * Returns a new `dns::HostName` object which represents the name of the DNS domain in which
     * this object resides.
     *
     * All domains of depth greater than 1 are composed of multiple sub-domains.  This function
     * provides the next-level parent of the domain represented by `*this`.
     *
     * PRE: This `dns::HostName` must have at least two subdomains (`this->depth() > 1`).
     *
     * NOTE: The behavior of this function is undefined unless its preconditions are met.
     *
     * RETURNS: A `dns::HostName` which has one fewer domain specified.
     */
    HostName parentDomain() const {
        if (this->_nameComponents.size() == 1) {
            uasserted(ErrorCodes::DNSRecordTypeMismatch,
                      "A top level domain has no subdomains in its name");
        }
        HostName result = *this;
        result._nameComponents.pop_back();
        return result;
    }

    /**
     * Returns true if the specified `candidate` HostName would be resolved within `*this` as a
     * hostname and false otherwise. That is, HostName can be obtained by applying `.parentDomain()`
     * to `candidate` at least once.
     *
     * Two domains can be said to have a "contains" relationship only when both are Fully
     * Qualified Domain Names (FQDNs).  When either domain or both domains are unqualified, then it
     * is impossible to know whether one could be resolved within the other correctly.
     *
     * THROWS: `DBException` with `ErrorCodes::DNSRecordTypeMismatch` as the status value unless
     * both this and candidate are fully-qualified.
     */
    bool contains(const HostName& candidate) const {
        if (!this->isFQDN() || !candidate.isFQDN()) {
            uasserted(ErrorCodes::DNSRecordTypeMismatch,
                      "Only FQDNs can be checked for subdomain relationships.");
        }
        // All of _nameComponents must appear as a prefix of candidate._nameComponents,
        // and candidate must have name components beyond that prefix.
        return (_nameComponents.size() < candidate._nameComponents.size()) &&
            std::equal(
                   begin(_nameComponents), end(_nameComponents), begin(candidate._nameComponents));
    }

    /**
     * Returns a new `dns::HostName` which represents the larger (possibly canonical) name that
     * would be used to lookup `*this` within the specified `domain`.  The result's
     * `canonicalName()` is the concatenation of this object's canonical name with the canonical
     * name of the specified `domain`.
     *
     * Unqualified hostnames can be prepended to other hostnames to provide a DNS string
     * which is equivalent to what a resolution of the unqualified name would be in the domain of
     * the second (possibly qualified) name.
     *
     * Throws an `DBException` with status `ErrorCodes::DNSRecordTypeMismatch` if this HostName is
     * fully-qualified.
     */
    HostName resolvedIn(const HostName& domain) const {
        if (this->_fullyQualified)
            uasserted(
                ErrorCodes::DNSRecordTypeMismatch,
                "A fully qualified Domain Name cannot be resolved within another domain name.");
        HostName result = domain;
        result._nameComponents.insert(
            end(result._nameComponents), begin(this->_nameComponents), end(this->_nameComponents));
        return result;
    }

    /**
     * The sequence of domain path components in top-first order (e.g. {"com","mongodb","www"}).
     */
    const std::vector<std::string>& nameComponents() const& {
        return this->_nameComponents;
    }

    std::vector<std::string> nameComponents() && {
        return std::move(this->_nameComponents);
    }

    /**
     * Returns true if `lhs` and `rhs` represent the same DNS path.
     *
     * In addition to the lookup sequence (order of sub domains) being the same, the
     * qualification of both objects must be the same.  For example, `"www.google.com"` would not
     * compare equal to `"www.google.com."` due to the presence of a trailing dot in the second
     * case.
     */
    friend bool operator==(const HostName& lhs, const HostName& rhs) {
        auto lens = [](const auto& h) { return std::tie(h._fullyQualified, h._nameComponents); };
        return lens(lhs) == lens(rhs);
    }

    friend bool operator!=(const HostName& lhs, const HostName& rhs) {
        return !(lhs == rhs);
    }

    /**
     * Streams a representation of the specified `hostName` to the specified `os` formatting stream.
     * Returns a reference to `os` for chaining.
     *
     * A canonical representation of `hostName` (with a trailing dot, `'.'`, when `hostName.isFQDN()
     * == true`) will be placed into the formatting stream.
     */
    friend std::ostream& operator<<(std::ostream& os, const HostName& hostName) {
        return hostName.stream(os);
    }

    friend StringBuilder& operator<<(StringBuilder& os, const HostName& hostName) {
        return hostName.stream(os);
    }

private:
    // All streaming functions boil down into this central handler, for both `StringBuilder` and
    // `std::ostream`.
    template <typename StreamLike>
    void streamUnqualified(StreamLike& os) const {
        StringData sep;
        std::for_each(
            rbegin(_nameComponents), rend(_nameComponents), [&sep, &os](const auto& component) {
                os << sep << component;
                sep = "."_sd;
            });
    }

    // When printing unqualified names to a stream, we omit the trailing dot, even if needed.
    // Always append a dot when printing fully qualified names.
    template <typename StreamLike>
    StreamLike& stream(StreamLike& os) const {
        streamUnqualified(os);
        if (_fullyQualified) {
            os << '.';
        }
        return os;
    }

    // Returns true only if this is not fully qualified, has exactly 4 name components, and
    // and consists solely of digits.
    bool isEquivalentToIPv4DottedDecimal() const {
        return !_fullyQualified && _nameComponents.size() == 4 &&
            std::all_of(begin(_nameComponents), end(_nameComponents), [](const auto& s) {
                return std::all_of(begin(s), end(s), [](char c) { return std::isdigit(c); });
            });
    }

    // Hostname components are stored top-first (e.g. "com","mongodb","www").
    // This is the reverse of how a name is read by humans in text form.
    std::vector<std::string> _nameComponents;

    // FQDNs and Relative Names are discriminated by this field.
    Qualification _fullyQualified;
};
}  // namespace dns
}  // namespace mongo
