/* 
 * Copyright (C) 2011 Jiaju Zhang <jjzhang@suse.de>
 * Copyright (C) 2013-2014 Philipp Marek <philipp.marek@linbit.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <string.h>
#include <stdlib.h>
#include <net/if.h>
#include <asm/types.h>
#include <linux/rtnetlink.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include "booth.h"
#include "inline-fn.h"
#include "log.h"
#include "config.h"
#include "ticket.h"
#include "transport.h"

#define BOOTH_IPADDR_LEN	(sizeof(struct in6_addr))

#define NETLINK_BUFSIZE		16384
#define SOCKET_BUFFER_SIZE	160000
#define FRAME_SIZE_MAX		10000



struct booth_site *local = NULL;


static int (*deliver_fn) (void *msg, int msglen);


static void parse_rtattr(struct rtattr *tb[],
			 int max, struct rtattr *rta, int len)
{
	while (RTA_OK(rta, len)) {
		if (rta->rta_type <= max)
			tb[rta->rta_type] = rta;
		rta = RTA_NEXT(rta,len);
	}
}

enum match_type {
	NO_MATCH = 0,
	FUZZY_MATCH,
	EXACT_MATCH,
};

static int find_address(unsigned char ipaddr[BOOTH_IPADDR_LEN],
		int family, int prefixlen,
		int fuzzy_allowed,
		struct booth_site **me,
		int *address_bits_matched)
{
	int i;
	struct booth_site *node;
	int bytes, bits_left, mask;
	unsigned char node_bits, ip_bits;
	uint8_t *n_a;
	int matched;
	enum match_type did_match = NO_MATCH;


	bytes = prefixlen / 8;
	bits_left = prefixlen % 8;
	/* One bit left to check means ignore 7 lowest bits. */
	mask = ~( (1 << (8 - bits_left)) -1);

	for (i = 0; i < booth_conf->site_count; i++) {
		node = booth_conf->site + i;
		if (family != node->family)
			continue;
		n_a = node_to_addr_pointer(node);

		for(matched = 0; matched < node->addrlen; matched++)
			if (ipaddr[matched] != n_a[matched])
				break;


		if (matched == node->addrlen) {
			/* Full match. */
			*address_bits_matched = matched * 8;
found:
			*me = node;
			did_match = EXACT_MATCH;
			continue;
		}

		if (!fuzzy_allowed)
			continue;


		/* Check prefix, whole bytes */
		if (matched < bytes)
			continue;
		if (matched * 8 < *address_bits_matched)
			continue;
		if (!bits_left)
			goto found;

		node_bits = n_a[bytes];
		ip_bits = ipaddr[bytes];
		if (((node_bits ^ ip_bits) & mask) == 0) {
			/* _At_least_ prefixlen bits matched. */
			*address_bits_matched = prefixlen;
			if (did_match < EXACT_MATCH) {
				*me = node;
				did_match = FUZZY_MATCH;
			}
		}
	}

	return did_match;
}


int _find_myself(int family, struct booth_site **mep, int fuzzy_allowed);
int _find_myself(int family, struct booth_site **mep, int fuzzy_allowed)
{
	int fd;
	struct sockaddr_nl nladdr;
	struct booth_site *me;
	unsigned char ipaddr[BOOTH_IPADDR_LEN];
	static char rcvbuf[NETLINK_BUFSIZE];
	struct {
		struct nlmsghdr nlh;
		struct rtgenmsg g;
	} req;
	int address_bits_matched;


	if (local)
		goto found;


	me = NULL;
	address_bits_matched = 0;
	if (mep)
		*mep = NULL;
	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0) {
		log_error("failed to create netlink socket");
		return 0;
	}

	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family = AF_NETLINK;

	memset(&req, 0, sizeof(req));
	req.nlh.nlmsg_len = sizeof(req);
	req.nlh.nlmsg_type = RTM_GETADDR;
	req.nlh.nlmsg_flags = NLM_F_ROOT|NLM_F_MATCH|NLM_F_REQUEST;
	req.nlh.nlmsg_pid = 0;
	req.nlh.nlmsg_seq = 1;
	req.g.rtgen_family = family;

	if (sendto(fd, (void *)&req, sizeof(req), 0,
				(struct sockaddr*)&nladdr, sizeof(nladdr)) < 0)  {
		close(fd);
		log_error("failed to send data to netlink socket");
		return 0;
	}

	while (1) {
		int status;
		struct nlmsghdr *h;
		struct iovec iov = { rcvbuf, sizeof(rcvbuf) };
		struct msghdr msg = {
			(void *)&nladdr, sizeof(nladdr),
			&iov,   1,
			NULL,   0,
			0
		};

		status = recvmsg(fd, &msg, 0);
		if (!status) {
			close(fd);
			log_error("failed to recvmsg from netlink socket");
			return 0;
		}

		h = (struct nlmsghdr *)rcvbuf;
		if (h->nlmsg_type == NLMSG_DONE)
			break;

		if (h->nlmsg_type == NLMSG_ERROR) {
			close(fd);
			log_error("netlink socket recvmsg error");
			return 0;
		}

		while (NLMSG_OK(h, status)) {
			if (h->nlmsg_type == RTM_NEWADDR) {
				struct ifaddrmsg *ifa = NLMSG_DATA(h);
				struct rtattr *tb[IFA_MAX+1];
				int len = h->nlmsg_len 
					- NLMSG_LENGTH(sizeof(*ifa));

				memset(tb, 0, sizeof(tb));
				parse_rtattr(tb, IFA_MAX, IFA_RTA(ifa), len);
				memset(ipaddr, 0, BOOTH_IPADDR_LEN);
				/* prefer IFA_LOCAL if it exists, for p-t-p
				 * interfaces, otherwise use IFA_ADDRESS */
				if (tb[IFA_LOCAL]) {
					memcpy(ipaddr, RTA_DATA(tb[IFA_LOCAL]),
							BOOTH_IPADDR_LEN);
				} else {
					memcpy(ipaddr, RTA_DATA(tb[IFA_ADDRESS]),
							BOOTH_IPADDR_LEN);
				}

				/* First try with exact addresses, then optionally with subnet matching. */
				if (ifa->ifa_prefixlen > address_bits_matched)
					find_address(ipaddr,
							ifa->ifa_family, ifa->ifa_prefixlen,
							fuzzy_allowed, &me, &address_bits_matched);
			}
			h = NLMSG_NEXT(h, status);
		}
	}

	close(fd);

	if (!me)
		return 0;

	me->local = 1;
	local = me;
found:
	if (mep)
		*mep = local;
	return 1;
}

int find_myself(struct booth_site **mep, int fuzzy_allowed)
{
	return _find_myself(AF_INET6, mep, fuzzy_allowed) ||
		_find_myself(AF_INET, mep, fuzzy_allowed);
}


/** Checks the header fields for validity.
 * cf. init_header().
 * For @len_incl_data < 0 the length is not checked.
 * Return <0 if error, else bytes read. */
int check_boothc_header(struct boothc_header *h, int len_incl_data)
{
	int l;

	if (h->magic != htonl(BOOTHC_MAGIC)) {
		log_error("magic error %x", ntohl(h->magic));
		return -EINVAL;
	}
	if (h->version != htonl(BOOTHC_VERSION)) {
		log_error("version error %x", ntohl(h->version));
		return -EINVAL;
	}


	l = ntohl(h->length);
	if (l < sizeof(*h)) {
		log_error("length %d out of range", l);
		return -EINVAL;
	}


	if (len_incl_data < 0)
		return 0;

	if (l != len_incl_data) {
		log_error("length error - got %d, wanted %d",
				l, len_incl_data);
		return -EINVAL;
	}

	return len_incl_data;
}


static void process_tcp_listener(int ci)
{
	int fd, i, one = 1;
	socklen_t addrlen = sizeof(struct sockaddr);
	struct sockaddr addr;

	fd = accept(clients[ci].fd, &addr, &addrlen);
	if (fd < 0) {
		log_error("process_tcp_listener: accept error %d %d",
			  fd, errno);
		return;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));


	i = client_add(fd, clients[ci].transport,
			process_connection, NULL);

	log_debug("client connection %d fd %d", i, fd);
}

int setup_tcp_listener(int test_only)
{
	int s, rv;
	int one = 1;

	s = socket(local->family, SOCK_STREAM, 0);
	if (s == -1) {
		log_error("failed to create tcp socket %s", strerror(errno));
		return s;
	}

	rv = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));
	if (rv == -1) {
		log_error("failed to set the SO_REUSEADDR option");
		return rv;
	}

	rv = bind(s, &local->sa6, local->saddrlen);
	if (test_only) {
		rv = (rv == -1) ? errno : 0;
		close(s);
		return rv;
	}

	if (rv == -1) {
		log_error("failed to bind socket %s", strerror(errno));
		return rv;
	}

	rv = listen(s, 5);
	if (rv == -1) {
		log_error("failed to listen on socket %s", strerror(errno));
		return rv;
	}

	return s;
}

static int booth_tcp_init(void * unused __attribute__((unused)))
{
	int rv;

	if (get_local_id() < 0)
		return -1;

	rv = setup_tcp_listener(0);
	if (rv < 0)
		return rv;

	client_add(rv, booth_transport + TCP,
			process_tcp_listener, NULL);

	return 0;
}

static int connect_nonb(int sockfd, const struct sockaddr *saptr,
			socklen_t salen, int sec)
{
	int		flags, n, error;
	socklen_t	len;
	fd_set		rset, wset;
	struct timeval	tval;

	flags = fcntl(sockfd, F_GETFL, 0);
	fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

	error = 0;
	if ( (n = connect(sockfd, saptr, salen)) < 0)
		if (errno != EINPROGRESS)
			return -1;

	if (n == 0)
		goto done;	/* connect completed immediately */

	FD_ZERO(&rset);
	FD_SET(sockfd, &rset);
	wset = rset;
	tval.tv_sec = sec;
	tval.tv_usec = 0;

	if ((n = select(sockfd + 1, &rset, &wset, NULL,
	    sec ? &tval : NULL)) == 0) {
		/* leave outside function to close */
		/* timeout */
		/* close(sockfd); */	
		errno = ETIMEDOUT;
		return -1;
	}

	if (FD_ISSET(sockfd, &rset) || FD_ISSET(sockfd, &wset)) {
		len = sizeof(error);
		if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
			return -1;	/* Solaris pending error */
	} else {
		log_error("select error: sockfd not set");
		return -1;
	}

done:
	fcntl(sockfd, F_SETFL, flags);	/* restore file status flags */

	if (error) {
		/* leave outside function to close */
		/* close(sockfd); */	
		errno = error;
		return -1;
	}

	return 0;
}

int booth_tcp_open(struct booth_site *to)
{
	int s, rv;

	if (to->tcp_fd >= STDERR_FILENO)
		goto found;

	s = socket(to->family, SOCK_STREAM, 0);
	if (s == -1) {
		log_error("cannot create socket of family %d", to->family);
		return -1;
	}


	rv = connect_nonb(s, (struct sockaddr *)&to->sa6, to->saddrlen, 10);
	if (rv == -1) {
		if( errno == ETIMEDOUT)
			log_error("connect to %s got a timeout", site_string(to));
		else 
			log_error("connect to %s got an error: %s", site_string(to),
					strerror(errno));
		goto error;
	}

	to->tcp_fd = s;

found:
	return 1;

error:
	if (s >= 0)
		close(s);
	return -1;
}

int booth_tcp_send(struct booth_site *to, void *buf, int len)
{
	return do_write(to->tcp_fd, buf, len);
}

static int booth_tcp_recv(struct booth_site *from, void *buf, int len)
{
	int got;
	/* Needs timeouts! */
	got = do_read(from->tcp_fd, buf, len);
	if (got < 0) {
		log_error("read failed (%d): %s", errno, strerror(errno));
		return got;
	}
	return len;
}

static int booth_tcp_close(struct booth_site *to)
{
	if (to) {
		if (to->tcp_fd > STDERR_FILENO)
			close(to->tcp_fd);
		to->tcp_fd = -1;
	}
	return 0;
}

static int booth_tcp_exit(void)
{
	return 0;
}

static int setup_udp_server(void)
{
	int rv, fd;
	int one = 1;
	unsigned int recvbuf_size;

	fd = socket(local->family, SOCK_DGRAM, 0);
	if (fd == -1) {
		log_error("failed to create UDP socket %s", strerror(errno));
		goto ex;
	}

	rv = fcntl(fd, F_SETFL, O_NONBLOCK);
	if (rv == -1) {
		log_error("failed to set non-blocking operation "
			  "on UDP socket: %s", strerror(errno));
		goto ex;
	}

	rv = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));
	if (rv == -1) {
		log_error("failed to set the SO_REUSEADDR option");
		goto ex;
	}

	rv = bind(fd, (struct sockaddr *)&local->sa6, local->saddrlen);

	if (rv == -1) {
		log_error("failed to bind UDP socket to [%s]:%d: %s",
				site_string(local), booth_conf->port,
				strerror(errno));
		goto ex;
	}

	recvbuf_size = SOCKET_BUFFER_SIZE;
	rv = setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
			&recvbuf_size, sizeof(recvbuf_size));
	if (rv == -1) {
		log_error("failed to set recvbuf size");
		goto ex;
	}

	local->udp_fd = fd;
	return 0;

ex:
	if (fd >= 0)
		close(fd);
	return -1;
}


/* Receive/process callback for UDP */
static void process_recv(int ci)
{
	struct sockaddr_storage sa;
	int rv;
	socklen_t sa_len;
	char buffer[256];
	/* Used for unit tests */
	struct boothc_ticket_msg *msg;


	sa_len = sizeof(sa);
	msg = (void*)buffer;
	rv = recvfrom(clients[ci].fd,
			buffer, sizeof(buffer),
			MSG_NOSIGNAL | MSG_DONTWAIT,
			(struct sockaddr *)&sa, &sa_len);
	if (rv == -1)
		return;

	deliver_fn(msg, rv);
}

static int booth_udp_init(void *f)
{
	int rv;

	rv = setup_udp_server();
	if (rv < 0)
		return rv;

	deliver_fn = f;
	client_add(local->udp_fd,
			booth_transport + UDP,
			process_recv, NULL);

	return 0;
}

int booth_udp_send(struct booth_site *to, void *buf, int len)
{
	int rv;

	rv = sendto(local->udp_fd, buf, len, MSG_NOSIGNAL,
			(struct sockaddr *)&to->sa6, to->saddrlen);
	if (rv == len) {
		rv = 0;
	} else if (rv < 0) {
		log_error("Cannot send to %s: %d %s",
				site_string(to),
				errno,
				strerror(errno));
	} else {
		rv = -1;
		log_error("Packet sent to %s got truncated",
				site_string(to));
	}

	return rv;
}

static int booth_udp_broadcast(void *buf, int len)
{
	int i, rv, rvs;
	struct booth_site *site;


	if (!booth_conf || !booth_conf->site_count)
		return -1;

	rvs = 0;
	foreach_node(i, site) {
		if (site != local) {
			rv = booth_udp_send(site, buf, len);
			if (!rvs)
				rvs = rv;
		}
	}

	return rvs;
}

static int booth_udp_exit(void)
{
	return 0;
}

/* SCTP transport layer has not been developed yet */
static int booth_sctp_init(void *f __attribute__((unused)))
{
	return 0;
}

static int booth_sctp_send(struct booth_site * to __attribute__((unused)),
			   void *buf __attribute__((unused)),
			   int len __attribute__((unused)))
{
	return 0;
}

static int booth_sctp_broadcast(void *buf __attribute__((unused)),
				int len __attribute__((unused)))
{
	return 0;
}

static int return_0_booth_site(struct booth_site *v __attribute((unused)))
{
	return 0;
}

static int return_0(void)
{
	return 0;
}
const struct booth_transport booth_transport[TRANSPORT_ENTRIES] = {
	[TCP] = {
		.name = "TCP",
		.init = booth_tcp_init,
		.open = booth_tcp_open,
		.send = booth_tcp_send,
		.recv = booth_tcp_recv,
		.close = booth_tcp_close,
		.exit = booth_tcp_exit
	},
	[UDP] = {
		.name = "UDP",
		.init = booth_udp_init,
		.open = return_0_booth_site,
		.send = booth_udp_send,
		.close = return_0_booth_site,
		.broadcast = booth_udp_broadcast,
		.exit = booth_udp_exit
	},
	[SCTP] = {
		.name = "SCTP",
		.init = booth_sctp_init,
		.open = return_0_booth_site,
		.send = booth_sctp_send,
		.broadcast = booth_sctp_broadcast,
		.exit = return_0,
	}
};

const struct booth_transport *local_transport = booth_transport+TCP;



int send_header_only(int fd, struct boothc_header *hdr)
{
	int rv;

	rv = do_write(fd, hdr, sizeof(*hdr));

	return rv;
}


int send_ticket_msg(int fd, struct boothc_ticket_msg *msg)
{
	int rv;

	rv = do_write(fd, msg, sizeof(*msg));

	return rv;
}


int send_header_plus(int fd, struct boothc_header *hdr, void *data, int len)
{
	int rv;
	int l;

	if (data == hdr->data) {
		l = sizeof(*hdr) + len;
		assert(l == ntohl(hdr->length));

		/* One struct */
		rv = do_write(fd, hdr, l);
	} else {
		/* Header and data in two locations */
		rv = send_header_only(fd, hdr);

		if (rv >= 0 && len)
			rv = do_write(fd, data, len);
	}

	return rv;
}
