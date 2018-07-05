/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <autoconf.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <utils/util.h>

#include <sel4/sel4.h>

#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>

#undef PACKED
#include <pico_bsd_sockets.h>

#include "../drivers/uart.h"
#include "../syscalls.h"

#define STDOUT_FD 1
#define STDERR_FD 2
#define PICO_FD_START 3

static void debug_put_char(UNUSED char c)
{
#if CONFIG_DEBUG_BUILD
    seL4_DebugPutChar(c);
#endif
}

static vputchar_t vputchar = debug_put_char;

static size_t output(void *data, size_t count)
{
    char *realdata = data;
    size_t i;
    for (i = 0; i < count; i++) {
        vputchar(realdata[i]);
    }
    return i;
}

void update_vputchar(vputchar_t v)
{
    vputchar = v;
}

long sys_writev(va_list ap)
{
    int fildes = va_arg(ap, int);
    struct iovec *iov = va_arg(ap, struct iovec *);
    int iovcnt = va_arg(ap, int);

    long long sum = 0;
    ssize_t ret = 0;

    /* The iovcnt argument is valid if greater than 0 and less than or equal to IOV_MAX. */
    if (iovcnt <= 0 || iovcnt > IOV_MAX) {
        return -EINVAL;
    }

    /* The sum of iov_len is valid if less than or equal to SSIZE_MAX i.e. cannot overflow
       a ssize_t. */
    for (int i = 0; i < iovcnt; i++) {
        sum += (long long)iov[i].iov_len;
        if (sum > SSIZE_MAX) {
            return -EINVAL;
        }
    }

    /* If all the iov_len members in the array are 0, return 0. */
    if (!sum) {
        return 0;
    }

    /* Write the buffer to console if the fd is for stdout or stderr. */
    if (fildes == STDOUT_FD || fildes == STDERR_FD) {
        for (int i = 0; i < iovcnt; i++) {
            ret += output(iov[i].iov_base, iov[i].iov_len);
        }
    } else if (fildes >= PICO_FD_START) {
        for (int i = 0; i < iovcnt; i++) {
            int res = pico_write(fildes - PICO_FD_START, iov[i].iov_base, iov[i].iov_len);
            if (res == -1) {
                return -errno;
            } else {
                ret += res;
            }
        }
    }

    return ret;
}

long sys_read(va_list ap)
{
    int fd = va_arg(ap, int);
    void *buf = va_arg(ap, void*);
    size_t count = va_arg(ap, size_t);
    /* construct an iovec and call readv */
    struct iovec iov = {.iov_base = buf, .iov_len = count };
    return readv(fd, &iov, 1);
}

long sys_ioctl(va_list ap)
{
    int fd = va_arg(ap, int);
    UNUSED int request = va_arg(ap, int);
    /* muslc does some ioctls to stdout, so just allow these to silently
       go through */
    if (fd == STDOUT_FD) {
        return 0;
    }

    ZF_LOGF("io ctl not implemented");
    return 0;
}

long sys_getuid(UNUSED va_list ap)
{
    return 0;
}

long sys_getgid(UNUSED va_list ap)
{
    return 0;
}

long sys_openat(UNUSED va_list ap)
{
    return ENOSYS;
}

long sys_socket(va_list ap)
{
    int domain = va_arg(ap, int);
    int type = va_arg(ap, int);
    int protocol = va_arg(ap, int);

    /* Call pico tcp */
    int new_sd = pico_newsocket(domain, type, protocol);
    if (new_sd < 0) {
        ZF_LOGE("failed to create new pico socket %d", errno);
        return -errno;
    }
    new_sd += PICO_FD_START;
    return new_sd;
}

long sys_bind(va_list ap)
{
    int sd = va_arg(ap, int);
    struct sockaddr *local_addr = va_arg(ap, struct sockaddr *);
    socklen_t socklen = va_arg(ap, socklen_t);

    if (sd >= PICO_FD_START) {
        int ret = pico_bind(sd - PICO_FD_START, local_addr, socklen);
        return ret == 0 ? 0 : -errno;
    }
    return -EINVAL;
}

long sys_listen(va_list ap) {
    int sd = va_arg(ap, int);
    int backlog = va_arg(ap, int);

    if (sd >= PICO_FD_START) {
        int ret = pico_listen(sd - PICO_FD_START, backlog);
        return ret == 0 ? 0 : -errno;
    }

    return -EINVAL;
}

long sys_connect(va_list ap)
{
    int sd = va_arg(ap, int);
    const struct sockaddr *_saddr = va_arg(ap, const struct sockaddr *);
    socklen_t socklen = va_arg(ap, socklen_t);

    if (sd >= PICO_FD_START) {
        if (pico_connect(sd - PICO_FD_START, _saddr, socklen) == 0) {
            return 0;
        } else if (errno == EAGAIN) {
            /* picoTCP reports EAGAIN instead of EINPROGRESS as an async connection return code.
             * As a workaround, treat EAGAIN the same as EINPROGRESS. */
            return -EINPROGRESS;
        } else {
            return -errno;
        }
    }
    return -EINVAL;
}

long sys_accept(va_list ap)
{
    int sd = va_arg(ap, int);
    struct sockaddr *_orig = va_arg(ap, struct sockaddr *);
    socklen_t *socklen = va_arg(ap, socklen_t *);

    if (sd >= PICO_FD_START) {
        int ret = pico_accept(sd - PICO_FD_START, _orig, socklen);
        return ret == 0 ? 0 : -errno;
    }
    return -EINVAL;
}

long sys_sendto(va_list ap)
{
    int sd = va_arg(ap, int);
    void *buf = va_arg(ap, void *);
    int len = va_arg(ap, int);
    int flags = va_arg(ap, int);
    struct sockaddr *_dst = va_arg(ap, struct sockaddr *);
    socklen_t socklen = va_arg(ap, socklen_t);

    if (sd >= PICO_FD_START) {
        int ret = pico_sendto(sd - PICO_FD_START, buf, len, flags, _dst, socklen);
        return ret < 0 ? -errno : ret;
    }
    return -EINVAL;
}

long sys_recvfrom(va_list ap)
{
    int sd = va_arg(ap, int);
    void *buf = va_arg(ap, void *);
    int len = va_arg(ap, int);
    int flags = va_arg(ap, int);
    struct sockaddr *_addr = va_arg(ap, struct sockaddr *);
    socklen_t *socklen = va_arg(ap, socklen_t *);

    if (sd >= PICO_FD_START) {
        int ret = pico_recvfrom(sd - PICO_FD_START, buf, len, flags, _addr, socklen);
        return ret >= 0 ? ret : -errno;
    }
    return -EINVAL;
}

long sys_readv(va_list ap)
{
    int fd = va_arg(ap, int);
    const struct iovec *iov = va_arg(ap, const struct iovec *);
    int iovcnt = va_arg(ap, int);

    if (fd >= PICO_FD_START) {
        int total = 0;
        for (int i = 0; i < iovcnt; i++) {
            int ret = pico_read(fd - PICO_FD_START, iov[i].iov_base, iov[i].iov_len);
            if (ret == -1) {
                break;
            } else {
                total += ret;
            }
        }
        return total == 0 ? -errno : total;
    }

    return -EINVAL;
}

long sys_close(va_list ap)
{
    int sockfd = va_arg(ap, int);
    if (sockfd >= PICO_FD_START) {
        int ret = pico_close(sockfd - PICO_FD_START);
        return ret == 0 ? 0 : -errno;
    }
    return -EINVAL;
}

long sys_getsockname(va_list ap)
{
    int sd = va_arg(ap, int);
    struct sockaddr *local_addr = va_arg(ap, struct sockaddr *);
    socklen_t *socklen = va_arg(ap, socklen_t *);

    if (sd >= PICO_FD_START) {
        int ret = pico_getsockname(sd - PICO_FD_START, local_addr, socklen);
        return ret == 0 ? 0 : -errno;
    }
    return -EINVAL;
}

long sys_getpeername(va_list ap)
{
    int sd = va_arg(ap, int);
    struct sockaddr *remote_addr = va_arg(ap, struct sockaddr *);
    socklen_t *socklen = va_arg(ap, socklen_t *);

    if (sd >= PICO_FD_START) {
        int ret = pico_getpeername(sd - PICO_FD_START, remote_addr, socklen);
        return ret == 0 ? 0 : -errno;
    }
    return -EINVAL;
}

long sys_fcntl(va_list ap)
{
    int sockfd = va_arg(ap, int);
    int cmd = va_arg(ap, int);
    int arg = va_arg(ap, int);
    if (sockfd >= PICO_FD_START) {
        int ret = pico_fcntl(sockfd - PICO_FD_START, cmd, arg);
        return ret == 0 ? 0 : -errno;
    }
    return -EINVAL;
}

long sys_setsockopt(va_list ap)
{
    int sockfd = va_arg(ap, int);
    int level = va_arg(ap, int);
    int optname = va_arg(ap, int);
    const void *optval = va_arg(ap, void *);
    socklen_t optlen = va_arg(ap, socklen_t);

    if (sockfd >= PICO_FD_START) {
        int ret = pico_setsockopt(sockfd - PICO_FD_START, level, optname, optval, optlen);
        return ret == 0 ? 0 : -errno;
    }
    return -EINVAL;
}

long sys_getsockopt(va_list ap)
{
    int sockfd = va_arg(ap, int);
    int level = va_arg(ap, int);
    int optname = va_arg(ap, int);
    void *optval = va_arg(ap, void *);
    socklen_t *optlen = va_arg(ap, socklen_t *);

    if (sockfd >= PICO_FD_START) {
        int err = pico_getsockopt(sockfd - PICO_FD_START, level, optname, optval, optlen);
        if (err == -1) {
            /* picoTCP reports EAGAIN even after a socket is correctly connected,
             * so we ignore EAGAIN error codes here. */
            return errno == EAGAIN ? 0 : -errno;
        } else {
            return 0;
        }
    }
    return -EINVAL;
}

long sys_ppoll(va_list ap)
{
    struct pollfd *pfd = va_arg(ap, struct pollfd *);
    nfds_t npfd = va_arg(ap, nfds_t);
    struct timespec *tmo_p = va_arg(ap, struct timespec *);

    if (npfd > RLIMIT_NOFILE) {
        return -EINVAL;
    }

    for (nfds_t i = 0; i < npfd; i++) {
        if (pfd[i].fd >= PICO_FD_START) {
            pfd[i].fd -= PICO_FD_START;
        } else {
            return -EINVAL;
        }
    }

    /* ignore timeouts, they won't work */
    int ret = pico_ppoll(pfd, npfd, tmo_p, NULL);

    for (nfds_t i = 0; i < npfd; i++) {
        pfd[i].fd += PICO_FD_START;
    }

    return ret >= 0 ? ret : -errno;
}
