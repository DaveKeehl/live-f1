/* live-f1
 *
 * stream.c - data stream and key frame parsing
 *
 * Copyright © 2005 Scott James Remnant <scott@netsplit.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif /* HAVE_CONFIG_H */


#include <sys/poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "live-f1.h"
#include "stream.h"


/* Which car the packet is for */
#define PACKET_CAR(_p) ((_p)[0] & 0x1f)

/* Which type of packet it is */
#define PACKET_TYPE(_p) (((_p)[0] >> 5) | (((_p)[1] & 0x01) << 3))

/* Data from a short packet */
#define SHORT_PACKET_DATA(_p) (((_p)[1] & 0x0e) >> 1)

/* Data from a special packet */
#define SPECIAL_PACKET_DATA(_p) ((_p)[1] >> 1)

/* Length of the packet if it's one of the long ones */
#define LONG_PACKET_LEN(_p) (SPECIAL_PACKET_DATA(_p) + 2)

/* Flag for a nominally short packet with no following data */
#define SHORT_PACKET_NUL(_p) (((_p)[1] & 0xf0) == 0xf0)

/* Length of the packet if it's one of the short ones */
#define SHORT_PACKET_LEN(_p) ((SHORT_PACKET_NUL(_p) ? 0 : ((_p)[1] >> 4)) + 2)

/* Length of the packet if it's a special one */
#define SPECIAL_PACKET_LEN(_p) 2


/* Types of packets for cars */
typedef enum {
	CAR_POSITION_UPDATE,
	CAR_POSITION,
	CAR_NUMBER,
	CAR_DRIVER,
	/* Everything else is short */
	CAR_POSITION_HISTORY = 15
} CarPacketType;

/* Types of non-car packets */
typedef enum {
	SYS_EVENT_ID = 1,
	SYS_KEY_FRAME,
	SYS_UNKNOWN_SPECIAL_A,
	SYS_UNKNOWN_LONG_A,
	SYS_UNKNOWN_SPECIAL_B,
	SYS_UNKNOWN_LONG_B,
	SYS_STRANGE_A, /* Always two bytes */
	SYS_UNKNOWN_SPECIAL_C,
	SYS_UNKNOWN_SHORT_A,
	SYS_UNKNOWN_LONG_C,
	SYS_UNKNOWN_SHORT_B,
	SYS_COPYRIGHT
} SystemPacketType;


/* Forward prototypes */
static int next_packet (unsigned char *packet, size_t *packet_len,
			const unsigned char **buf, size_t *buf_len);


/**
 * open_stream:
 * @hostname: hostname of timing server,
 * @port: port of timing server.
 *
 * Creates a socket for the data stream and connects to the live timing
 * server so data can be received.
 *
 * Returns: connected socket or -1 on failure.
 **/
int
open_stream (const char   *hostname,
	     unsigned int  port)
{
	struct addrinfo *res, *addr, hints;
	static char      service[6];
	int              sock, ret;

	info (2, _("Looking up %s ...\n"), hostname);

	sprintf (service, "%hu", port);

	memset (&hints, 0, sizeof (hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_CANONNAME;

	ret = getaddrinfo (hostname, service, &hints, &res);
	if (ret != 0) {
		fprintf (stderr, "%s: %s: %s: %s\n", program_name,
			 _("failed to resolve host"), hostname,
			 gai_strerror (ret));
		return -1;
	}

	info (1, _("Connecting to data stream ...\n"));

	sock = -1;
	for (addr = res; addr; addr = addr->ai_next) {
		info(3, _("Trying %s ...\n"), addr->ai_canonname);

		sock = socket (addr->ai_family, addr->ai_socktype,
			       addr->ai_protocol);
		if (sock < 0)
			continue;

		ret = connect (sock, addr->ai_addr, addr->ai_addrlen);
		if (ret < 0) {
			close (sock);
			sock = -1;
			continue;
		}

		break;
	}

	if (sock < 0) {
		fprintf (stderr, "%s; %s: %s\n", program_name,
			 _("failed to connect to data stream"),
			 strerror (errno));

		freeaddrinfo (res);
		return -1;
	}

	info (3, _("Connected to %s.\n"), addr->ai_canonname);
	return sock;
}

/**
 * read_stream:
 * @http_sess: neon http session to use for key frames,
 * @sock: data stream socket to read from.
 *
 * Read a block of data from the stream, this isn't quite as simple as it
 * seems because the server won't actually send us data unless we ping it;
 * but we don't want to ping as often as we need to check for things like
 * key presses from the user.
 *
 * Returns: 0 if socket closed, > 0 on success, < 0 on error.
 **/
int
read_stream (ne_session *http_sess,
	     int         sock)
{
	struct pollfd poll_fd;
	static int    timer = 0;
	int           ret;

	poll_fd.fd = sock;
	poll_fd.events = POLLIN;
	poll_fd.revents = 0;

	ret = poll (&poll_fd, 1, 100);
	if (ret < 0) {
		goto error;
	} else if (ret == 0) {
		char buf[1];

		if (timer++ < 10)
			return 1;

		info (3, _("Sending ping ...\n"));

		/* Wake the server up */
		buf[0] = 0x10;
		ret = write (sock, buf, sizeof (buf));
		if (ret < 0)
			goto error;

		timer = 0;
		return 1;
	}

	/* Error condition */
	if (poll_fd.revents & (POLLERR | POLLNVAL))
		goto error;

	/* Server went away */
	if (poll_fd.revents & POLLHUP) {
		close (sock);
		return 0;
	}

	/* Yay, data! */
	if (poll_fd.revents & POLLIN) {
		unsigned char buf[512];

		ret = read (sock, buf, sizeof (buf));
		if (ret < 0) {
			goto error;
		} else if (ret == 0) {
			close (sock);
			return 0;
		}

		parse_stream_block (http_sess, buf, ret);
	}

	timer = 0;
	return ret;
error:
	close (sock);
	return -1;
}

/**
 * parse_stream_block:
 * @http_sess: neon http session to use for key frames,
 * @buf: data read from server or key frame,
 * @buf_len: length of @buf.
 *
 * Parse a data stream block obtained either from the data server or a
 * key frame.
 **/
void
parse_stream_block (ne_session          *http_sess,
		    const unsigned char *buf,
		    size_t               buf_len)
{
	static unsigned char packet[129];
	static size_t        packet_len = 0;

	while (next_packet (packet, &packet_len, &buf, &buf_len)) {
		/*info (3, _("PACKET! %zi bytes\n"), packet_len); */

		packet_len = 0;
	}
}

/**
 * next_packet:
 * @packet: buffer in which to store the packet,
 * @packet_len: length of packet data already in @packet,
 * @buf: buffer to copy packet from,
 * @buf_len: length of @buf.
 *
 * Copy a packet, or part thereof, from @buf into @packet updating
 * @packet_len to the new length.  Can be called a byte at a time if
 * that's how the packet arrives.
 *
 * Returns: 0 if the packet was not complete, 1 if it is complete
 **/
static int
next_packet (unsigned char        *packet,
	     size_t               *packet_len,
	     const unsigned char **buf,
	     size_t               *buf_len)
{
	size_t needed, expected;

	/* We need a minimum of two bytes to figure out how long the rest
	 * of it's supposed to be; copy those now if we have room.
	 */
	if (*packet_len < 2) {
		needed = MIN (*buf_len, 2 - *packet_len);
		memcpy (packet + *packet_len, *buf, needed);

		*packet_len += needed;
		*buf += needed;
		*buf_len -= needed;
		if (*packet_len < 2)
			return 0;
	}

	/* We have enough of the packet to know how long it is */
	if (PACKET_CAR (packet)) {
		switch ((CarPacketType) PACKET_TYPE (packet)) {
		case CAR_POSITION_UPDATE:
			expected = SPECIAL_PACKET_LEN (packet);
			break;
		case CAR_POSITION_HISTORY:
			expected = LONG_PACKET_LEN (packet);
			break;
		default:
			expected = SHORT_PACKET_LEN (packet);
			break;
		}
	} else {
		switch ((SystemPacketType) PACKET_TYPE (packet)) {
		case SYS_UNKNOWN_SPECIAL_A:
		case SYS_UNKNOWN_SPECIAL_B:
		case SYS_UNKNOWN_SPECIAL_C:
			expected = SPECIAL_PACKET_LEN (packet);
			break;
		case SYS_EVENT_ID:
		case SYS_KEY_FRAME:
		case SYS_UNKNOWN_SHORT_A:
		case SYS_UNKNOWN_SHORT_B:
			expected = SHORT_PACKET_LEN (packet);
			break;
		case SYS_STRANGE_A:
			expected = 4;
			break;
		default:
			expected = LONG_PACKET_LEN (packet);
			break;
		}
	}

	/* Copy as much as we can */
	needed = MIN (*buf_len, expected - *packet_len);
	memcpy (packet + *packet_len, *buf, needed);

	*packet_len += needed;
	*buf += needed;
	*buf_len -= needed;

	return (*packet_len == expected);
}
