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

#pragma once

#include <poll.h>

#include <sys/socket.h>
#include <sys/types.h>

namespace mongo::transport {

/**
 * Wrappers around system functions used by `//src/mongo/transport/handoff:*` components.
 * Rather than call a system function directly (e.g. `getegid()`), call it through an instance of
 * `POSIXInterface` (e.g. `_posix.getegid()`).
 * The default implementations of `POSIXInterface` member functions call the underlying libc
 * functions. Unit tests override `POSIXInterface` functions to simulate failures.
 */
class POSIXInterface {
public:
    virtual ~POSIXInterface() = default;

    virtual int accept(int socket, struct sockaddr* address, socklen_t* address_len);
    virtual int bind(int socket, const struct sockaddr* address, socklen_t address_len);
    virtual int chmod(const char* path, mode_t mode);
    virtual int chown(const char* path, uid_t owner, gid_t group);
    virtual int close(int fildes);
    virtual int fcntl(int fildes, int cmd);
    virtual int fcntl(int fildes, int cmd, int arg);
    virtual gid_t getegid();
    virtual int getpeereid(int socket, uid_t* euid, gid_t* egid);
    virtual int getpeername(int socket, struct sockaddr* address, socklen_t* address_len);
    virtual int getsockname(int socket, struct sockaddr* address, socklen_t* address_len);
    virtual int listen(int socket, int backlog);
    virtual int pipe(int fildes[2]);
    virtual int poll(struct pollfd* fds, nfds_t nfds, int timeout);
    virtual ssize_t recv(int socket, void* buffer, size_t length, int flags);
    virtual ssize_t recvmsg(int socket, struct msghdr* message, int flags);
    virtual ssize_t send(int socket, const void* buffer, size_t length, int flags);
    virtual int setsockopt(
        int socket, int level, int option_name, const void* option_value, socklen_t option_len);
    virtual int shutdown(int socket, int how);
    virtual int socket(int domain, int type, int protocol);
    virtual int unlink(const char* path);
    virtual ssize_t write(int fildes, const void* buf, size_t nbyte);
};

}  // namespace mongo::transport
