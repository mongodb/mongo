// message_port.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/message_port.h"

#include <fcntl.h>
#include <time.h>

#include "mongo/config.h"
#include "mongo/util/allocator.h"
#include "mongo/util/background.h"
#include "mongo/util/log.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#ifndef _WIN32
#ifndef __sun
#include <ifaddrs.h>
#endif
#include <sys/resource.h>
#include <sys/stat.h>
#endif

namespace mongo {

using std::shared_ptr;
using std::string;

// if you want trace output:
#define mmm(x)

void AbstractMessagingPort::setConnectionId(long long connectionId) {
    verify(_connectionId == 0);
    _connectionId = connectionId;
}

/* messagingport -------------------------------------------------------------- */

class PiggyBackData {
public:
    PiggyBackData(MessagingPort* port) {
        _port = port;
        _buf = new char[1300];
        _cur = _buf;
    }

    ~PiggyBackData() {
        DESTRUCTOR_GUARD(flush(); delete[](_cur););
    }

    void append(Message& m) {
        verify(m.header().getLen() <= 1300);

        if (len() + m.header().getLen() > 1300)
            flush();

        memcpy(_cur, m.singleData().view2ptr(), m.header().getLen());
        _cur += m.header().getLen();
    }

    void flush() {
        if (_buf == _cur)
            return;

        _port->send(_buf, len(), "flush");
        _cur = _buf;
    }

    int len() const {
        return _cur - _buf;
    }

private:
    MessagingPort* _port;
    char* _buf;
    char* _cur;
};

class Ports {
    std::set<MessagingPort*> ports;
    stdx::mutex m;

public:
    Ports() : ports() {}
    void closeAll(unsigned skip_mask) {
        stdx::lock_guard<stdx::mutex> bl(m);
        for (std::set<MessagingPort*>::iterator i = ports.begin(); i != ports.end(); i++) {
            if ((*i)->tag & skip_mask)
                continue;
            (*i)->shutdown();
        }
    }
    void insert(MessagingPort* p) {
        stdx::lock_guard<stdx::mutex> bl(m);
        ports.insert(p);
    }
    void erase(MessagingPort* p) {
        stdx::lock_guard<stdx::mutex> bl(m);
        ports.erase(p);
    }
};

// we "new" this so it is still be around when other automatic global vars
// are being destructed during termination.
Ports& ports = *(new Ports());

void MessagingPort::closeAllSockets(unsigned mask) {
    ports.closeAll(mask);
}

MessagingPort::MessagingPort(int fd, const SockAddr& remote)
    : psock(new Socket(fd, remote)), piggyBackData(0) {
    ports.insert(this);
}

MessagingPort::MessagingPort(double timeout, logger::LogSeverity ll)
    : psock(new Socket(timeout, ll)) {
    ports.insert(this);
    piggyBackData = 0;
}

MessagingPort::MessagingPort(std::shared_ptr<Socket> sock) : psock(sock), piggyBackData(0) {
    ports.insert(this);
}

void MessagingPort::setSocketTimeout(double timeout) {
    psock->setTimeout(timeout);
}

void MessagingPort::shutdown() {
    psock->close();
}

MessagingPort::~MessagingPort() {
    if (piggyBackData)
        delete (piggyBackData);
    shutdown();
    ports.erase(this);
}

bool MessagingPort::recv(Message& m) {
    try {
#ifdef MONGO_CONFIG_SSL
    again:
#endif
        // mmm( log() << "*  recv() sock:" << this->sock << endl; )
        MSGHEADER::Value header;
        int headerLen = sizeof(MSGHEADER::Value);
        psock->recv((char*)&header, headerLen);
        int len = header.constView().getMessageLength();

        if (len == 542393671) {
            // an http GET
            string msg =
                "It looks like you are trying to access MongoDB over HTTP on the native driver "
                "port.\n";
            LOG(psock->getLogLevel()) << msg;
            std::stringstream ss;
            ss << "HTTP/1.0 200 OK\r\nConnection: close\r\nContent-Type: "
                  "text/plain\r\nContent-Length: " << msg.size() << "\r\n\r\n" << msg;
            string s = ss.str();
            send(s.c_str(), s.size(), "http");
            return false;
        }
        // If responseTo is not 0 or -1 for first packet assume SSL
        else if (psock->isAwaitingHandshake()) {
#ifndef MONGO_CONFIG_SSL
            if (header.constView().getResponseTo() != 0 &&
                header.constView().getResponseTo() != -1) {
                uasserted(17133,
                          "SSL handshake requested, SSL feature not available in this build");
            }
#else
            if (header.constView().getResponseTo() != 0 &&
                header.constView().getResponseTo() != -1) {
                uassert(17132,
                        "SSL handshake received but server is started without SSL support",
                        sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled);
                setX509SubjectName(
                    psock->doSSLHandshake(reinterpret_cast<const char*>(&header), sizeof(header)));
                psock->setHandshakeReceived();
                goto again;
            }
            uassert(17189,
                    "The server is configured to only allow SSL connections",
                    sslGlobalParams.sslMode.load() != SSLParams::SSLMode_requireSSL);
#endif  // MONGO_CONFIG_SSL
        }
        if (static_cast<size_t>(len) < sizeof(MSGHEADER::Value) ||
            static_cast<size_t>(len) > MaxMessageSizeBytes) {
            LOG(0) << "recv(): message len " << len << " is invalid. "
                   << "Min " << sizeof(MSGHEADER::Value) << " Max: " << MaxMessageSizeBytes;
            return false;
        }

        psock->setHandshakeReceived();
        int z = (len + 1023) & 0xfffffc00;
        verify(z >= len);
        MsgData::View md = reinterpret_cast<char*>(mongoMalloc(z));
        ScopeGuard guard = MakeGuard(free, md.view2ptr());
        verify(md.view2ptr());

        memcpy(md.view2ptr(), &header, headerLen);
        int left = len - headerLen;

        psock->recv(md.data(), left);

        guard.Dismiss();
        m.setData(md.view2ptr(), true);
        return true;

    } catch (const SocketException& e) {
        logger::LogSeverity severity = psock->getLogLevel();
        if (!e.shouldPrint())
            severity = severity.lessSevere();
        LOG(severity) << "SocketException: remote: " << remote() << " error: " << e;
        m.reset();
        return false;
    }
}

void MessagingPort::reply(Message& received, Message& response) {
    say(/*received.from, */ response, received.header().getId());
}

void MessagingPort::reply(Message& received, Message& response, MSGID responseTo) {
    say(/*received.from, */ response, responseTo);
}

bool MessagingPort::call(Message& toSend, Message& response) {
    mmm(log() << "*call()" << endl;) say(toSend);
    return recv(toSend, response);
}

bool MessagingPort::recv(const Message& toSend, Message& response) {
    while (1) {
        bool ok = recv(response);
        if (!ok) {
            mmm(log() << "recv not ok" << endl;) return false;
        }
        // log() << "got response: " << response.data->responseTo << endl;
        if (response.header().getResponseTo() == toSend.header().getId())
            break;
        error() << "MessagingPort::call() wrong id got:" << std::hex
                << (unsigned)response.header().getResponseTo()
                << " expect:" << (unsigned)toSend.header().getId() << '\n' << std::dec
                << "  toSend op: " << (unsigned)toSend.operation() << '\n'
                << "  response msgid:" << (unsigned)response.header().getId() << '\n'
                << "  response len:  " << (unsigned)response.header().getLen() << '\n'
                << "  response op:  " << response.operation() << '\n'
                << "  remote: " << psock->remoteString();
        verify(false);
        response.reset();
    }
    mmm(log() << "*call() end" << endl;) return true;
}

void MessagingPort::say(Message& toSend, int responseTo) {
    verify(!toSend.empty());
    mmm(log() << "*  say()  thr:" << GetCurrentThreadId() << endl;)
        toSend.header().setId(nextMessageId());
    toSend.header().setResponseTo(responseTo);

    if (piggyBackData && piggyBackData->len()) {
        mmm(log() << "*     have piggy back"
                  << endl;) if ((piggyBackData->len() + toSend.header().getLen()) > 1300) {
            // won't fit in a packet - so just send it off
            piggyBackData->flush();
        }
        else {
            piggyBackData->append(toSend);
            piggyBackData->flush();
            return;
        }
    }

    toSend.send(*this, "say");
}

void MessagingPort::piggyBack(Message& toSend, int responseTo) {
    if (toSend.header().getLen() > 1300) {
        // not worth saving because its almost an entire packet
        say(toSend);
        return;
    }

    // we're going to be storing this, so need to set it up
    toSend.header().setId(nextMessageId());
    toSend.header().setResponseTo(responseTo);

    if (!piggyBackData)
        piggyBackData = new PiggyBackData(this);

    piggyBackData->append(toSend);
}

HostAndPort MessagingPort::remote() const {
    if (!_remoteParsed.hasPort()) {
        SockAddr sa = psock->remoteAddr();
        _remoteParsed = HostAndPort(sa.getAddr(), sa.getPort());
    }
    return _remoteParsed;
}

SockAddr MessagingPort::remoteAddr() const {
    return psock->remoteAddr();
}

SockAddr MessagingPort::localAddr() const {
    return psock->localAddr();
}

}  // namespace mongo
