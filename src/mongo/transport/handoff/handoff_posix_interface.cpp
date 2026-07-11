// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
