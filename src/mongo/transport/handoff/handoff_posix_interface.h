// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
