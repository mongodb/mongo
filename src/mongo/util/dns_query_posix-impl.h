/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#ifndef MONGO_UTIL_DNS_QUERY_PLATFORM_INCLUDE_WHITELIST
#error Do not include the DNS Query platform implementation headers.  Please use "mongo/util/dns_query.h" instead.
#endif

// DNS Headers for POSIX/libresolv have to be included in a specific order
// clang-format off
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
// clang-format on

#include <stdio.h>

#include <iostream>
#include <cassert>
#include <sstream>
#include <string>
#include <cstdint>
#include <vector>
#include <array>
#include <stdexcept>
#include <memory>
#include <exception>

#include <boost/noncopyable.hpp>

namespace mongo {
namespace dns {
// The anonymous namespace is safe, in this header, as it is not really a header.  It is only used
// in the `dns_query.cpp` TU.
namespace {

using std::begin;
using std::end;
using namespace std::literals::string_literals;

const std::size_t kMaxExpectedDNSResponseSize = 65536;
const std::size_t kMaxSRVHostNameSize = 8192;

enum class DNSQueryClass {
    kInternet = ns_c_in,
};

enum class DNSQueryType {
    kSRV = ns_t_srv,
    kTXT = ns_t_txt,
    kAddress = ns_t_a,
};

/**
 * A `ResourceRecord` represents a single DNS entry as parsed by the resolver API.
 * It can be viewed as one of various record types, using the member functions.
 * It roughly corresponds to the DNS RR data structure
 */
class ResourceRecord {
public:
    explicit ResourceRecord() = default;

    explicit ResourceRecord(std::string initialService, ns_msg& ns_answer, const int initialPos)
        : _service(std::move(initialService)),
          _answerStart(ns_msg_base(ns_answer)),
          _answerEnd(ns_msg_end(ns_answer)),
          _pos(initialPos) {
        if (ns_parserr(&ns_answer, ns_s_an, initialPos, &this->_resource_record))
            this->_badRecord();
    }

    /**
     * View this record as a DNS TXT record.
     */
    std::vector<std::string> txtEntry() const {
        const auto data = this->_rawData();
        if (data.empty()) {
            uasserted(ErrorCodes::DNSProtocolError, "DNS TXT Record is not correctly sized");
        }
        const std::size_t amount = data.front();
        const auto first = begin(data) + 1;
        std::vector<std::string> rv;
        if (data.size() - 1 < amount) {
            uasserted(ErrorCodes::DNSProtocolError, "DNS TXT Record is not correctly sized");
        }
        rv.emplace_back(first, first + amount);
        return rv;
    }

    /**
     * View this record as a DNS A record.
     */
    std::string addressEntry() const {
        std::string rv;

        auto data = _rawData();
        if (data.size() != 4) {
            uasserted(ErrorCodes::DNSProtocolError, "DNS A Record is not correctly sized");
        }
        for (const std::uint8_t& ch : data) {
            std::ostringstream oss;
            oss << int(ch);
            rv += oss.str() + ".";
        }
        rv.pop_back();
        return rv;
    }

    /**
     * View this record as a DNS SRV record.
     */
    SRVHostEntry srvHostEntry() const {
        const std::size_t kPortOffsetInPacket = 4;

        const std::uint8_t* const data = ns_rr_rdata(this->_resource_record);
        if (data < this->_answerStart ||
            data + kPortOffsetInPacket + sizeof(std::uint16_t) > this->_answerEnd) {
            std::ostringstream oss;
            oss << "Invalid record " << this->_pos << " of SRV answer for \"" << this->_service
                << "\": Incorrect result size";
            uasserted(ErrorCodes::DNSProtocolError, oss.str());
        }
        const std::uint16_t port = [data] {
            std::uint16_t tmp;
            memcpy(&tmp, data + kPortOffsetInPacket, sizeof(tmp));
            return ntohs(tmp);
        }();

        // The '@' is an impermissible character in a host name, so we populate the string we'll
        // return with it, such that a failure in string manipulation or corrupted dns packets will
        // cause an illegal hostname.
        std::string name(kMaxSRVHostNameSize, '@');

        const auto size = dn_expand(this->_answerStart,
                                    this->_answerEnd,
                                    data + kPortOffsetInPacket + sizeof(port),
                                    &name[0],
                                    name.size());

        if (size < 1)
            this->_badRecord();

        // Trim the expanded name
        name.resize(name.find('\0'));
        name += '.';

        // return by copy is equivalent to a `shrink_to_fit` and `move`.
        return {name, port};
    }

private:
    void _badRecord() const {
        std::ostringstream oss;
        oss << "Invalid record " << this->_pos << " of DNS answer for \"" << this->_service
            << "\": \"" << strerror(errno) << "\"";
        uasserted(ErrorCodes::DNSProtocolError, oss.str());
    };

    std::vector<std::uint8_t> _rawData() const {
        const std::uint8_t* const data = ns_rr_rdata(this->_resource_record);
        const std::size_t length = ns_rr_rdlen(this->_resource_record);

        return {data, data + length};
    }

    std::string _service;
    ns_rr _resource_record;
    const std::uint8_t* _answerStart;
    const std::uint8_t* _answerEnd;
    int _pos;
};

/**
 * The `DNSResponse` class represents a response to a DNS query.
 * It has STL-compatible iterators to view individual DNS Resource Records within a response.
 */
class DNSResponse {
public:
    explicit DNSResponse(std::string initialService, std::vector<std::uint8_t> initialData)
        : _service(std::move(initialService)), _data(std::move(initialData)) {
        if (ns_initparse(this->_data.data(), this->_data.size(), &this->_ns_answer)) {
            std::ostringstream oss;
            oss << "Invalid SRV answer for \"" << this->_service << "\"";
            uasserted(ErrorCodes::DNSProtocolError, oss.str());
        }

        this->_nRecords = ns_msg_count(this->_ns_answer, ns_s_an);

        if (!this->_nRecords) {
            std::ostringstream oss;
            oss << "No SRV records for \"" << this->_service << "\"";
            uasserted(ErrorCodes::DNSProtocolError, oss.str());
        }
    }

    class iterator {
    public:
        auto makeRelopsLens() const {
            return std::tie(this->_response, this->_pos);
        }

        inline friend bool operator==(const iterator& lhs, const iterator& rhs) {
            return lhs.makeRelopsLens() == rhs.makeRelopsLens();
        }

        inline friend bool operator<(const iterator& lhs, const iterator& rhs) {
            return lhs.makeRelopsLens() < rhs.makeRelopsLens();
        }

        inline friend bool operator!=(const iterator& lhs, const iterator& rhs) {
            return !(lhs == rhs);
        }

        const ResourceRecord& operator*() {
            this->_populate();
            return this->_record;
        }

        const ResourceRecord* operator->() {
            this->_populate();
            return &this->_record;
        }

        iterator& operator++() {
            this->_advance();
            return *this;
        }

        iterator operator++(int) {
            iterator tmp = *this;
            this->_advance();
            return tmp;
        }

    private:
        friend DNSResponse;

        explicit iterator(DNSResponse* const r)
            : _response(r), _record(this->_response->_service, this->_response->_ns_answer, 0) {}

        explicit iterator(DNSResponse* const initialResponse, const int initialPos)
            : _response(initialResponse), _pos(initialPos) {}

        void _populate() {
            if (this->_ready) {
                return;
            }
            this->_record =
                ResourceRecord(this->_response->_service, this->_response->_ns_answer, this->_pos);
            this->_ready = true;
        }

        void _advance() {
            ++this->_pos;
            this->_ready = false;
        }

        DNSResponse* _response;
        int _pos = 0;
        ResourceRecord _record;
        bool _ready = false;
    };

    auto begin() {
        return iterator(this);
    }

    auto end() {
        return iterator(this, this->_nRecords);
    }

    std::size_t size() const {
        return this->_nRecords;
    }

private:
    std::string _service;
    std::vector<std::uint8_t> _data;
    ns_msg _ns_answer;
    std::size_t _nRecords;
};

/**
 * The `DNSQueryState` object represents the state of a DNS query interface, on Unix-like systems.
 */
class DNSQueryState : boost::noncopyable {
public:
    std::vector<std::uint8_t> raw_lookup(const std::string& service,
                                         const DNSQueryClass class_,
                                         const DNSQueryType type) {
        std::vector<std::uint8_t> result(kMaxExpectedDNSResponseSize);
        const int size = res_nsearch(
            &_state, service.c_str(), int(class_), int(type), &result[0], result.size());

        if (size < 0) {
            std::ostringstream oss;
            oss << "Failed to look up service \"" << service << "\": " << strerror(errno);
            uasserted(ErrorCodes::DNSHostNotFound, oss.str());
        }
        result.resize(size);

        return result;
    }

    DNSResponse lookup(const std::string& service,
                       const DNSQueryClass class_,
                       const DNSQueryType type) {
        return DNSResponse(service, raw_lookup(service, class_, type));
    }

public:
    ~DNSQueryState() {
        res_nclose(&_state);
    }

    DNSQueryState() : _state() {
        res_ninit(&_state);
    }

private:
    struct __res_state _state;
};
}  // namespace
}  // namespace dns
}  // namespace mongo
