/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/transport/asio/asio_tcp_fast_open.h"

#include <array>
#include <fmt/format.h>
#include <fstream>
#include <map>
#include <string>
#include <utility>

#ifdef __linux__
#include <netinet/tcp.h>
#endif

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/transport_options_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/static_immortal.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::tfo {
namespace {

#ifdef TCP_FASTOPEN
using TcpFastOpenOption = SocketOption<IPPROTO_TCP, TCP_FASTOPEN>;
#endif

/**
 * On systems with TCP_FASTOPEN_CONNECT (linux >= 4.11),
 * we can get TFO "for free" by letting the kernel handle
 * postponing connect() until the first send() call.
 *
 * https://github.com/torvalds/linux/commit/19f6d3f3c8422d65b5e3d2162e30ef07c6e21ea2
 */
#ifdef TCP_FASTOPEN_CONNECT
using TcpFastOpenConnectOption = SocketOption<IPPROTO_TCP, TCP_FASTOPEN_CONNECT>;
#endif

constexpr std::array tcpFastOpenParameters{
    "tcpFastOpenServer",
    "tcpFastOpenClient",
    "tcpFastOpenQueueSize",
};

struct Config {
    void serialize(BSONObjBuilder* bob) const {
        bob->append("passive", passive);
        bob->append("client", clientEnabled);
        bob->append("server", serverEnabled);
        bob->append("queueSize", queueSize);
        {
            BSONObjBuilder sub{bob->subobjStart("initError")};
            initError.serialize(&sub);
        }
    }

    /**
     * in "passive mode", the user is assumed to be unaware of tfo, and
     * it will be attempted if available, but any errors related to its use
     * will be suppressed.
     */
    bool passive;

    bool clientEnabled;
    bool serverEnabled;
    int queueSize;

    /** Unfiltered status encountered from TFO initialization. */
    Status initError = Status::OK();
};

/**
 * Returns true iff any tfo-related parameters exist in the "setParameter"
 * startup option, regardless of the value to which they're set.
 */
bool anyOptionsPresent() {
    const auto& opts = optionenvironment::startupOptionsParsed;
    optionenvironment::Value pVal;
    if (opts.get("setParameter", &pVal).isOK()) {
        auto pMap = pVal.as<std::map<std::string, std::string>>();
        for (auto&& key : tcpFastOpenParameters)
            if (pMap.count(key))
                return true;
    }
    return false;
}

/** Makes an unbound TCP socket and tries to set an option on it, returning true on success. */
bool tryTcpSockOpt(int opt, int val) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        auto ec = lastSocketError();
        LOGV2_WARNING(5128700, "socket", "error"_attr = errorMessage(ec));
        return false;
    }
    int e = setsockopt(sock, IPPROTO_TCP, opt, reinterpret_cast<char*>(&val), sizeof(val));
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    return e == 0;
}

/**
 * Probe the socket API support for TFO-related options on TCP sockets, and
 * record the results in the global `networkCounter` object.
 */
void checkRelevantSocketOptionsAccepted() {
#ifdef TCP_FASTOPEN
    networkCounter.setTFOServerSupport(tryTcpSockOpt(TCP_FASTOPEN, 1));
#endif
#ifdef TCP_FASTOPEN_CONNECT
    networkCounter.setTFOClientSupport(tryTcpSockOpt(TCP_FASTOPEN_CONNECT, 1));
#endif
}

Config* configForTest = nullptr;

Config* realConfig() {
    static StaticImmortal<Config> instance{};
    return &*instance;
}

Config* config() {
    if (configForTest) {
        return configForTest;
    }
    return realConfig();
}

/** Errors are mapped to success in passive mode. */
std::error_code errorUnlessPassive(std::error_code ec) {
    if (config()->passive)
        return {};
    return ec;
}

/** Errors are mapped to success in passive mode. */
Status errorUnlessPassive(Status st) {
    if (config()->passive)
        return Status::OK();
    return st;
}

void checkSupportedByLibc(bool srv, bool cli) {
#ifndef TCP_FASTOPEN
    iassert(ErrorCodes::BadValue,
            "TCP FastOpen server support unavailable in this build of MongoDB",
            !srv);
#endif
#ifndef TCP_FASTOPEN_CONNECT
    iassert(ErrorCodes::BadValue,
            "TCP FastOpen client support unavailable in this build of MongoDB",
            !cli);
#endif
}

void checkEnabledByKernel(bool srv, bool cli) {
#if defined(TCP_FASTOPEN) && defined(__linux__)
    using namespace fmt::literals;
    if (!srv && !cli)
        return;
    std::string procfile("/proc/sys/net/ipv4/tcp_fastopen");
    boost::system::error_code ec;
    iassert(ErrorCodes::BadValue,
            "Unable to locate {}: {}"_format(procfile, errorCodeToStatus(ec).toString()),
            boost::filesystem::exists(procfile, ec));
    std::fstream f(procfile, std::ifstream::in);
    iassert(ErrorCodes::BadValue, "Unable to read {}"_format(procfile), f.is_open());

    int64_t k;  // The kernel setting.
    f >> k;
    networkCounter.setTFOKernelSetting(k);

    // Return an integer composed of all bits from 'm' that are missing from 'x'.
    auto maskBitsMissing = [](uint64_t x, uint64_t m) {
        return (x & m) ^ m;
    };
    uint64_t effCliMask = cli << 0;
    uint64_t effSrvMask = srv << 1;
    iassert(ErrorCodes::BadValue,
            "TCP FastOpen disabled in kernel. Set {} to {}"_format(procfile,
                                                                   k | effCliMask | effSrvMask),
            !maskBitsMissing(k, effCliMask) && !maskBitsMissing(k, effSrvMask));
#endif
}

void logInitializationMessage(Status st) {
    if (config()->passive) {
        // Implicit TCP FastOpen messaging.
        if (st.isOK()) {
            LOGV2(4648602, "Implicit TCP FastOpen in use.");
        } else {
            LOGV2(4648601,
                  "Implicit TCP FastOpen unavailable. If TCP FastOpen is "
                  "required, set at least one of the related parameters",
                  "relatedParameters"_attr = tcpFastOpenParameters);
        }
    } else {
        if (!st.isOK()) {
            LOGV2_WARNING_OPTIONS(23014,
                                  {logv2::LogTag::kStartupWarnings},
                                  "Failed to enable TCP Fast Open",
                                  "error"_attr = st);
        }
    }
}

void initializeOnce(Config& c) {
    Status st = Status::OK();
    try {
        c.passive = !anyOptionsPresent();
        c.clientEnabled = gTCPFastOpenClient;
        c.serverEnabled = gTCPFastOpenServer;
        c.queueSize = gTCPFastOpenQueueSize;

        checkSupportedByLibc(c.serverEnabled, c.clientEnabled);
        checkEnabledByKernel(c.serverEnabled, c.clientEnabled);
        checkRelevantSocketOptionsAccepted();
    } catch (const DBException& ex) {
        st = ex.toStatus().withContext("Unable to enable TCP FastOpen");
    }
    logInitializationMessage(st);
    c.initError = std::move(st);
}

}  // namespace

std::error_code initOutgoingSocket(AsioSession::GenericSocket& sock) {
    std::error_code ec;
#ifdef TCP_FASTOPEN_CONNECT
    setSocketOption(sock,
                    TcpFastOpenConnectOption(config()->clientEnabled),
                    "connect TCP fast open",
                    logv2::LogSeverity::Info(),
                    ec);
#endif
    return errorUnlessPassive(ec);
}

std::error_code initAcceptorSocket(AsioTransportLayer::GenericAcceptor& acceptor) {
    std::error_code ec;
#ifdef TCP_FASTOPEN
    const Config* c = config();
    LOGV2_DEBUG(7097402, 1, "tfo::initAcceptorSocket", "config"_attr = *c);
    if (c->serverEnabled) {
        setSocketOption(acceptor,
                        TcpFastOpenOption(c->queueSize),
                        "acceptor TCP fast open",
                        logv2::LogSeverity::Info(),
                        ec);
    }
#endif
    return errorUnlessPassive(ec);
}

Status ensureInitialized(bool returnUnfilteredError) {
    Status st = [] {
        if (configForTest)
            return configForTest->initError;
        [[maybe_unused]] static int realInitOnceDummy = [] {
            initializeOnce(*realConfig());
            return 0;
        }();
        return realConfig()->initError;
    }();
    if (!returnUnfilteredError)
        st = errorUnlessPassive(std::move(st));
    return st;
}

std::shared_ptr<void> setConfigForTest(
    bool passive, bool clientEnabled, bool serverEnabled, int queueSize, Status initError) {
    class Override {
    public:
        explicit Override(Config conf) : _c{std::move(conf)} {
            LOGV2(7097400, "TCPFastOpen config override begin", "config"_attr = _c);
            invariant(!configForTest);
            configForTest = &_c;
        }
        ~Override() {
            LOGV2(7097401, "TCPFastOpen config override end");
            configForTest = nullptr;
        }

    private:
        Config _c;
    };
    return std::make_shared<Override>(
        Config{passive, clientEnabled, serverEnabled, queueSize, std::move(initError)});
}

}  // namespace mongo::transport::tfo
