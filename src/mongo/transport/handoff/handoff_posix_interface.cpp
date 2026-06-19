/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/transport/handoff/handoff_posix_interface.h"

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

#include <sys/param.h>
#include <sys/stat.h>

namespace mongo::transport {

int POSIXInterface::accept(int socket, struct sockaddr* address, socklen_t* address_len) {
    return ::accept(socket, address, address_len);
}

int POSIXInterface::bind(int socket, const struct sockaddr* address, socklen_t address_len) {
    return ::bind(socket, address, address_len);
}

int POSIXInterface::chmod(const char* path, mode_t mode) {
    return ::chmod(path, mode);
}

int POSIXInterface::chown(const char* path, uid_t owner, gid_t group) {
    return ::chown(path, owner, group);
}

int POSIXInterface::close(int fildes) {
    return ::close(fildes);
}

int POSIXInterface::fcntl(int fildes, int cmd) {
    return ::fcntl(fildes, cmd);
}

int POSIXInterface::fcntl(int fildes, int cmd, int arg) {
    return ::fcntl(fildes, cmd, arg);
}

gid_t POSIXInterface::getegid() {
    return ::getegid();
}

int POSIXInterface::getpeereid(int socket, uid_t* euid, gid_t* egid) {
#ifdef BSD
    return ::getpeereid(socket, euid, egid);
#elif defined(__linux__)
    ::ucred cred;
    socklen_t len = sizeof(cred);
    const int rc = ::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &cred, &len);
    if (rc == 0) {
        *euid = cred.uid;
        *egid = cred.gid;
    }
    return rc;
#else
    (void)socket;
    (void)euid;
    (void)egid;
    errno = ENOSYS;
    return -1;
#endif
}

int POSIXInterface::getpeername(int socket, struct sockaddr* address, socklen_t* address_len) {
    return ::getpeername(socket, address, address_len);
}

int POSIXInterface::getsockname(int socket, struct sockaddr* address, socklen_t* address_len) {
    return ::getsockname(socket, address, address_len);
}

int POSIXInterface::listen(int socket, int backlog) {
    return ::listen(socket, backlog);
}

int POSIXInterface::pipe(int fildes[2]) {
    return ::pipe(fildes);
}

int POSIXInterface::poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    return ::poll(fds, nfds, timeout);
}

ssize_t POSIXInterface::recv(int socket, void* buffer, size_t length, int flags) {
    return ::recv(socket, buffer, length, flags);
}

ssize_t POSIXInterface::recvmsg(int socket, struct msghdr* message, int flags) {
    return ::recvmsg(socket, message, flags);
}

ssize_t POSIXInterface::send(int socket, const void* buffer, size_t length, int flags) {
    return ::send(socket, buffer, length, flags);
}

int POSIXInterface::setsockopt(
    int socket, int level, int option_name, const void* option_value, socklen_t option_len) {
    return ::setsockopt(socket, level, option_name, option_value, option_len);
}

int POSIXInterface::shutdown(int socket, int how) {
    return ::shutdown(socket, how);
}

int POSIXInterface::socket(int domain, int type, int protocol) {
    return ::socket(domain, type, protocol);
}

int POSIXInterface::unlink(const char* path) {
    return ::unlink(path);
}

ssize_t POSIXInterface::write(int fildes, const void* buf, size_t nbyte) {
    return ::write(fildes, buf, nbyte);
}

}  // namespace mongo::transport
