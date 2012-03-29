/*
 * gen_uuid.c --- generate a DCE-compatible uuid
 *
 * Copyright (C) 1996, 1997, 1998, 1999 Theodore Ts'o.
 *
 * %Begin-Header%
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * %End-Header%
 */

#include <stdlib.h>

/*
 * Force inclusion of SVID stuff since we need it if we're compiling in
 * gcc-wall wall mode
 */
#define _SVID_SOURCE

#ifdef _WIN32
#define _WIN32_WINNT 0x0500
#include <windows.h>
#define UUID MYUUID
#endif
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/wait.h>
#include <sys/stat.h>
#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_SYS_SOCKIO_H
#include <sys/sockio.h>
#endif
#ifdef HAVE_NET_IF_H
#include <net/if.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_NET_IF_DL_H
#include <net/if_dl.h>
#endif
#if defined(__linux__) && defined(HAVE_SYS_SYSCALL_H)
#include <sys/syscall.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include "uuidP.h"
#include "uuidd.h"

#ifdef HAVE_SRANDOM
#define srand(x)	srandom(x)
#define rand()		random()
#endif

#ifdef HAVE_TLS
#define THREAD_LOCAL static __thread
#else
#define THREAD_LOCAL static
#endif

#if defined(__linux__) && defined(__NR_gettid) && defined(HAVE_JRAND48)
#define DO_JRAND_MIX
THREAD_LOCAL unsigned short jrand_seed[3];
#endif

#ifdef _WIN32
static void gettimeofday (struct timeval *tv, void *dummy)
{
	FILETIME	ftime;
	uint64_t	n;

	GetSystemTimeAsFileTime (&ftime);
	n = (((uint64_t) ftime.dwHighDateTime << 32)
	     + (uint64_t) ftime.dwLowDateTime);
	if (n) {
		n /= 10;
		n -= ((369 * 365 + 89) * (uint64_t) 86400) * 1000000;
	}

	tv->tv_sec = n / 1000000;
	tv->tv_usec = n % 1000000;
}

static int getuid (void)
{
	return 1;
}
#endif

static int get_random_fd(void)
{
	struct timeval	tv;
	static int	fd = -2;
	int		i;

	if (fd == -2) {
		gettimeofday(&tv, 0);
#ifndef _WIN32
		fd = open("/dev/urandom", O_RDONLY);
		if (fd == -1)
			fd = open("/dev/random", O_RDONLY | O_NONBLOCK);
		if (fd >= 0) {
			i = fcntl(fd, F_GETFD);
			if (i >= 0)
				fcntl(fd, F_SETFD, i | FD_CLOEXEC);
		}
#endif
		srand((getpid() << 16) ^ getuid() ^ tv.tv_sec ^ tv.tv_usec);
#ifdef DO_JRAND_MIX
		jrand_seed[0] = getpid() ^ (tv.tv_sec & 0xFFFF);
		jrand_seed[1] = getppid() ^ (tv.tv_usec & 0xFFFF);
		jrand_seed[2] = (tv.tv_sec ^ tv.tv_usec) >> 16;
#endif
	}
	/* Crank the random number generator a few times */
	gettimeofday(&tv, 0);
	for (i = (tv.tv_sec ^ tv.tv_usec) & 0x1F; i > 0; i--)
		rand();
	return fd;
}


/*
 * Generate a series of random bytes.  Use /dev/urandom if possible,
 * and if not, use srandom/random.
 */
static void get_random_bytes(void *buf, int nbytes)
{
	int i, n = nbytes, fd = get_random_fd();
	int lose_counter = 0;
	unsigned char *cp = (unsigned char *) buf;

	if (fd >= 0) {
		while (n > 0) {
			i = read(fd, cp, n);
			if (i <= 0) {
				if (lose_counter++ > 16)
					break;
				continue;
			}
			n -= i;
			cp += i;
			lose_counter = 0;
		}
	}

	/*
	 * We do this all the time, but this is the only source of
	 * randomness if /dev/random/urandom is out to lunch.
	 */
	for (cp = buf, i = 0; i < nbytes; i++)
		*cp++ ^= (rand() >> 7) & 0xFF;

#ifdef DO_JRAND_MIX
	{
		unsigned short tmp_seed[3];

		memcpy(tmp_seed, jrand_seed, sizeof(tmp_seed));
		jrand_seed[2] = jrand_seed[2] ^ syscall(__NR_gettid);
		for (cp = buf, i = 0; i < nbytes; i++)
			*cp++ ^= (jrand48(tmp_seed) >> 7) & 0xFF;
		memcpy(jrand_seed, tmp_seed,
		       sizeof(jrand_seed)-sizeof(unsigned short));
	}
#endif

	return;
}

/*
 * Get the ethernet hardware address, if we can find it...
 *
 * XXX for a windows version, probably should use GetAdaptersInfo:
 * http://www.codeguru.com/cpp/i-n/network/networkinformation/article.php/c5451
 * commenting out get_node_id just to get gen_uuid to compile under windows
 * is not the right way to go!
 */
static int get_node_id(unsigned char *node_id)
{
#ifdef HAVE_NET_IF_H
	int		sd;
	struct ifreq	ifr, *ifrp;
	struct ifconf	ifc;
	char buf[1024];
	int		n, i;
	unsigned char	*a;
#ifdef HAVE_NET_IF_DL_H
	struct sockaddr_dl *sdlp;
#endif

/*
 * BSD 4.4 defines the size of an ifreq to be
 * max(sizeof(ifreq), sizeof(ifreq.ifr_name)+ifreq.ifr_addr.sa_len
 * However, under earlier systems, sa_len isn't present, so the size is
 * just sizeof(struct ifreq)
 */
#ifdef HAVE_SA_LEN
#ifndef max
#define max(a,b) ((a) > (b) ? (a) : (b))
#endif
#define ifreq_size(i) max(sizeof(struct ifreq),\
     sizeof((i).ifr_name)+(i).ifr_addr.sa_len)
#else
#define ifreq_size(i) sizeof(struct ifreq)
#endif /* HAVE_SA_LEN */

	sd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sd < 0) {
		return -1;
	}
	memset(buf, 0, sizeof(buf));
	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = buf;
	if (ioctl (sd, SIOCGIFCONF, (char *)&ifc) < 0) {
		close(sd);
		return -1;
	}
	n = ifc.ifc_len;
	for (i = 0; i < n; i+= ifreq_size(*ifrp) ) {
		ifrp = (struct ifreq *)((char *) ifc.ifc_buf+i);
		strncpy(ifr.ifr_name, ifrp->ifr_name, IFNAMSIZ);
#ifdef SIOCGIFHWADDR
		if (ioctl(sd, SIOCGIFHWADDR, &ifr) < 0)
			continue;
		a = (unsigned char *) &ifr.ifr_hwaddr.sa_data;
#else
#ifdef SIOCGENADDR
		if (ioctl(sd, SIOCGENADDR, &ifr) < 0)
			continue;
		a = (unsigned char *) ifr.ifr_enaddr;
#else
#ifdef HAVE_NET_IF_DL_H
		sdlp = (struct sockaddr_dl *) &ifrp->ifr_addr;
		if ((sdlp->sdl_family != AF_LINK) || (sdlp->sdl_alen != 6))
			continue;
		a = (unsigned char *) &sdlp->sdl_data[sdlp->sdl_nlen];
#else
		/*
		 * XXX we don't have a way of getting the hardware
		 * address
		 */
		close(sd);
		return 0;
#endif /* HAVE_NET_IF_DL_H */
#endif /* SIOCGENADDR */
#endif /* SIOCGIFHWADDR */
		if (!a[0] && !a[1] && !a[2] && !a[3] && !a[4] && !a[5])
			continue;
		if (node_id) {
			memcpy(node_id, a, 6);
			close(sd);
			return 1;
		}
	}
	close(sd);
#endif
	return 0;
}

/* Assume that the gettimeofday() has microsecond granularity */
#define MAX_ADJUSTMENT 10

static int get_clock(uint32_t *clock_high, uint32_t *clock_low,
		     uint16_t *ret_clock_seq, int *num)
{
	THREAD_LOCAL int		adjustment = 0;
	THREAD_LOCAL struct timeval	last = {0, 0};
	THREAD_LOCAL int		state_fd = -2;
	THREAD_LOCAL FILE		*state_f;
	THREAD_LOCAL uint16_t		clock_seq;
	struct timeval			tv;
	struct flock			fl;
	uint64_t			clock_reg;
	mode_t				save_umask;
	int				len;

	if (state_fd == -2) {
		save_umask = umask(0);
		state_fd = open("/var/lib/libuuid/clock.txt",
				O_RDWR|O_CREAT, 0660);
		(void) umask(save_umask);
		state_f = fdopen(state_fd, "r+");
		if (!state_f) {
			close(state_fd);
			state_fd = -1;
		}
	}
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	fl.l_pid = 0;
	if (state_fd >= 0) {
		rewind(state_f);
		while (fcntl(state_fd, F_SETLKW, &fl) < 0) {
			if ((errno == EAGAIN) || (errno == EINTR))
				continue;
			fclose(state_f);
			close(state_fd);
			state_fd = -1;
			break;
		}
	}
	if (state_fd >= 0) {
		unsigned int cl;
		unsigned long tv1, tv2;
		int a;

		if (fscanf(state_f, "clock: %04x tv: %lu %lu adj: %d\n",
			   &cl, &tv1, &tv2, &a) == 4) {
			clock_seq = cl & 0x3FFF;
			last.tv_sec = tv1;
			last.tv_usec = tv2;
			adjustment = a;
		}
	}

	if ((last.tv_sec == 0) && (last.tv_usec == 0)) {
		get_random_bytes(&clock_seq, sizeof(clock_seq));
		clock_seq &= 0x3FFF;
		gettimeofday(&last, 0);
		last.tv_sec--;
	}

try_again:
	gettimeofday(&tv, 0);
	if ((tv.tv_sec < last.tv_sec) ||
	    ((tv.tv_sec == last.tv_sec) &&
	     (tv.tv_usec < last.tv_usec))) {
		clock_seq = (clock_seq+1) & 0x3FFF;
		adjustment = 0;
		last = tv;
	} else if ((tv.tv_sec == last.tv_sec) &&
	    (tv.tv_usec == last.tv_usec)) {
		if (adjustment >= MAX_ADJUSTMENT)
			goto try_again;
		adjustment++;
	} else {
		adjustment = 0;
		last = tv;
	}

	clock_reg = tv.tv_usec*10 + adjustment;
	clock_reg += ((uint64_t) tv.tv_sec)*10000000;
	clock_reg += (((uint64_t) 0x01B21DD2) << 32) + 0x13814000;

	if (num && (*num > 1)) {
		adjustment += *num - 1;
		last.tv_usec += adjustment / 10;
		adjustment = adjustment % 10;
		last.tv_sec += last.tv_usec / 1000000;
		last.tv_usec = last.tv_usec % 1000000;
	}

	if (state_fd > 0) {
		rewind(state_f);
		len = fprintf(state_f,
			      "clock: %04x tv: %016lu %08lu adj: %08d\n",
			      clock_seq, last.tv_sec, last.tv_usec, adjustment);
		fflush(state_f);
		if (ftruncate(state_fd, len) < 0) {
			fprintf(state_f, "                   \n");
			fflush(state_f);
		}
		rewind(state_f);
		fl.l_type = F_UNLCK;
		fcntl(state_fd, F_SETLK, &fl);
	}

	*clock_high = clock_reg >> 32;
	*clock_low = clock_reg;
	*ret_clock_seq = clock_seq;
	return 0;
}

#if defined(HAVE_UUIDD) && defined(HAVE_SYS_UN_H)
/* used in get_uuid_via_daemon() only */
static ssize_t read_all(int fd, char *buf, size_t count)
{
	ssize_t ret;
	ssize_t c = 0;
	int tries = 0;

	memset(buf, 0, count);
	while (count > 0) {
		ret = read(fd, buf, count);
		if (ret <= 0) {
			if ((errno == EAGAIN || errno == EINTR || ret == 0) &&
			    (tries++ < 5))
				continue;
			return c ? c : -1;
		}
		if (ret > 0)
			tries = 0;
		count -= ret;
		buf += ret;
		c += ret;
	}
	return c;
}

/*
 * Close all file descriptors
 */
static void close_all_fds(void)
{
	int i, max;

#if defined(HAVE_SYSCONF) && defined(_SC_OPEN_MAX)
	max = sysconf(_SC_OPEN_MAX);
#elif defined(HAVE_GETDTABLESIZE)
	max = getdtablesize();
#elif defined(HAVE_GETRLIMIT) && defined(RLIMIT_NOFILE)
	struct rlimit rl;

	getrlimit(RLIMIT_NOFILE, &rl);
	max = rl.rlim_cur;
#else
	max = OPEN_MAX;
#endif

	for (i=0; i < max; i++) {
		close(i);
		if (i <= 2)
			open("/dev/null", O_RDWR);
	}
}

/*
 * Try using the uuidd daemon to generate the UUID
 *
 * Returns 0 on success, non-zero on failure.
 */
static int get_uuid_via_daemon(int op, uuid_t out, int *num)
{
	char op_buf[64];
	int op_len;
	int s;
	ssize_t ret;
	int32_t reply_len = 0, expected = 16;
	struct sockaddr_un srv_addr;
	struct stat st;
	pid_t pid;
	static const char *uuidd_path = UUIDD_PATH;
	static int access_ret = -2;
	static int start_attempts = 0;

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;

	srv_addr.sun_family = AF_UNIX;
	strcpy(srv_addr.sun_path, UUIDD_SOCKET_PATH);

	if (connect(s, (const struct sockaddr *) &srv_addr,
		    sizeof(struct sockaddr_un)) < 0) {
		if (access_ret == -2)
			access_ret = access(uuidd_path, X_OK);
		if (access_ret == 0)
			access_ret = stat(uuidd_path, &st);
		if (access_ret == 0 && (st.st_mode & (S_ISUID | S_ISGID)) == 0)
			access_ret = access(UUIDD_DIR, W_OK);
		if (access_ret == 0 && start_attempts++ < 5) {
			if ((pid = fork()) == 0) {
				close_all_fds();
				execl(uuidd_path, "uuidd", "-qT", "300",
				      (char *) NULL);
				exit(1);
			}
			(void) waitpid(pid, 0, 0);
			if (connect(s, (const struct sockaddr *) &srv_addr,
				    sizeof(struct sockaddr_un)) < 0)
				goto fail;
		} else
			goto fail;
	}
	op_buf[0] = op;
	op_len = 1;
	if (op == UUIDD_OP_BULK_TIME_UUID) {
		memcpy(op_buf+1, num, sizeof(*num));
		op_len += sizeof(*num);
		expected += sizeof(*num);
	}

	ret = write(s, op_buf, op_len);
	if (ret < 1)
		goto fail;

	ret = read_all(s, (char *) &reply_len, sizeof(reply_len));
	if (ret < 0)
		goto fail;

	if (reply_len != expected)
		goto fail;

	ret = read_all(s, op_buf, reply_len);

	if (op == UUIDD_OP_BULK_TIME_UUID)
		memcpy(op_buf+16, num, sizeof(int));

	memcpy(out, op_buf, 16);

	close(s);
	return ((ret == expected) ? 0 : -1);

fail:
	close(s);
	return -1;
}

#else /* !defined(HAVE_UUIDD) && defined(HAVE_SYS_UN_H) */
static int get_uuid_via_daemon(int op, uuid_t out, int *num)
{
	return -1;
}
#endif

void uuid__generate_time(uuid_t out, int *num)
{
	static unsigned char node_id[6];
	static int has_init = 0;
	struct uuid uu;
	uint32_t	clock_mid;

	if (!has_init) {
		if (get_node_id(node_id) <= 0) {
			get_random_bytes(node_id, 6);
			/*
			 * Set multicast bit, to prevent conflicts
			 * with IEEE 802 addresses obtained from
			 * network cards
			 */
			node_id[0] |= 0x01;
		}
		has_init = 1;
	}
	get_clock(&clock_mid, &uu.time_low, &uu.clock_seq, num);
	uu.clock_seq |= 0x8000;
	uu.time_mid = (uint16_t) clock_mid;
	uu.time_hi_and_version = ((clock_mid >> 16) & 0x0FFF) | 0x1000;
	memcpy(uu.node, node_id, 6);
	uuid_pack(&uu, out);
}

void uuid_generate_time(uuid_t out)
{
#ifdef HAVE_TLS
	THREAD_LOCAL int		num = 0;
	THREAD_LOCAL struct uuid	uu;
	THREAD_LOCAL time_t		last_time = 0;
	time_t				now;

	if (num > 0) {
		now = time(0);
		if (now > last_time+1)
			num = 0;
	}
	if (num <= 0) {
		num = 1000;
		if (get_uuid_via_daemon(UUIDD_OP_BULK_TIME_UUID,
					out, &num) == 0) {
			last_time = time(0);
			uuid_unpack(out, &uu);
			num--;
			return;
		}
		num = 0;
	}
	if (num > 0) {
		uu.time_low++;
		if (uu.time_low == 0) {
			uu.time_mid++;
			if (uu.time_mid == 0)
				uu.time_hi_and_version++;
		}
		num--;
		uuid_pack(&uu, out);
		return;
	}
#else
	if (get_uuid_via_daemon(UUIDD_OP_TIME_UUID, out, 0) == 0)
		return;
#endif

	uuid__generate_time(out, 0);
}


void uuid__generate_random(uuid_t out, int *num)
{
	uuid_t	buf;
	struct uuid uu;
	int i, n;

	if (!num || !*num)
		n = 1;
	else
		n = *num;

	for (i = 0; i < n; i++) {
		get_random_bytes(buf, sizeof(buf));
		uuid_unpack(buf, &uu);

		uu.clock_seq = (uu.clock_seq & 0x3FFF) | 0x8000;
		uu.time_hi_and_version = (uu.time_hi_and_version & 0x0FFF)
			| 0x4000;
		uuid_pack(&uu, out);
		out += sizeof(uuid_t);
	}
}

void uuid_generate_random(uuid_t out)
{
	int	num = 1;
	/* No real reason to use the daemon for random uuid's -- yet */

	uuid__generate_random(out, &num);
}


/*
 * This is the generic front-end to uuid_generate_random and
 * uuid_generate_time.  It uses uuid_generate_random only if
 * /dev/urandom is available, since otherwise we won't have
 * high-quality randomness.
 */
void uuid_generate(uuid_t out)
{
	if (get_random_fd() >= 0)
		uuid_generate_random(out);
	else
		uuid_generate_time(out);
}
