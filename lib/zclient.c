/* Zebra's client library.
 * Copyright (C) 1999 Kunihiro Ishiguro
 * Copyright (C) 2005 Andrew J. Schorr
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2, or (at your
 * option) any later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 */

#include <zebra.h>

#include "prefix.h"
#include "stream.h"
#include "buffer.h"
#include "network.h"
#include "if.h"
#include "log.h"
#include "thread.h"
#include "zclient.h"
#include "memory.h"
#include "table.h"

/* Zebra client events. */
enum event {ZLOOKUP_SCHEDULE, ZCLIENT_SCHEDULE, ZCLIENT_READ, ZCLIENT_CONNECT};

char *zclient_serv_path = NULL;

/* This file local debug flag. */
int zclient_debug = 0;

/* Nexus to use, if any */
static qpn_nexus zclient_nexus = NULL;

/* prototypes */
static int zclient_read (struct zclient *zclient);
static void zclient_event (enum event, struct zclient *);
static void zclient_event_r (enum event event, struct zclient *zclient);
static void zclient_event_t (enum event event, struct zclient *zclient);
static void zclient_connect_r (qtimer qtr, void* timer_info, qtime_t when);

/* Allocate zclient structure. */
struct zclient *
zclient_new ()
{
  struct zclient *zclient;
  zclient = XCALLOC (MTYPE_ZCLIENT, sizeof (struct zclient));

  zclient->sock = -1;

  zclient->ibuf = stream_new (ZEBRA_MAX_PACKET_SIZ);
  zclient->obuf = stream_new (ZEBRA_MAX_PACKET_SIZ);
  zclient->wb   = buffer_new(0);

  if (zclient_nexus)
    {
      zclient->qf  = qps_file_init_new(zclient->qf, NULL);
      zclient->qtr = qtimer_init_new(zclient->qtr, zclient_nexus->pile,
                                      zclient_connect_r, zclient);
    }

  return zclient;
}

/* This function is only called when exiting, because
   many parts of the code do not check for I/O errors, so they could
   reference an invalid pointer if the structure was ever freed.

   Free zclient structure. */
void
zclient_free (struct zclient *zclient)
{
  zclient_stop (zclient) ;      /* make sure stopped, socket closed etc */

  stream_free(zclient->ibuf);   /* Free, if any */
  stream_free(zclient->obuf);   /* Free, if any */
  if (zclient->wb)
    buffer_free(zclient->wb);

  /* qfile and qtimer */
  zclient->qf  = qps_file_free(zclient->qf) ;   /* Free, if any         */
  zclient->qtr = qtimer_free(zclient->qtr) ;    /* Free, if any         */

  XFREE (MTYPE_ZCLIENT, zclient);
}

/* Initialize to use a nexus (qpselect etc). */
void
zclient_init_r (qpn_nexus n)
{
  zclient_nexus = n;
}

/* Initialize zebra client.  Argument redist_default is unwanted
 * redistribute route type.
 *
 * Assumes is given a struct zclient* recently created by zclient_new(), or
 * recently stopped by zclient_stop().
 */
void
zclient_init (struct zclient *zclient, int redist_default)
{
  int i;

  /* Enable zebra client connection by default. */
  zclient->enable = 1;

  /* Set -1 to the default socket value.        */
  zclient->sock = -1;

  /* Clear redistribution flags.                */
  for (i = 0; i < ZEBRA_ROUTE_MAX; i++)
    zclient->redist[i] = 0;

  /* No failures, yet                           */
  zclient->fail = 0;

  /* Set unwanted redistribute route.  bgpd does not need BGP route
     redistribution. */
  zclient->redist_default = redist_default;
  zclient->redist[redist_default] = 1;

  /* Set default-information redistribute to zero. */
  zclient->default_information = 0;

  /* Schedule first zclient connection.         */
  if (zclient_debug)
    zlog_debug ("zclient start scheduled");

  zclient_event (ZCLIENT_SCHEDULE, zclient);
}

/* Schedule lookup connection */
void
zlookup_schedule(struct zclient *zclient)
{
  zclient_event (ZLOOKUP_SCHEDULE, zclient);
}

/* Stop zebra client services. */
void
zclient_stop (struct zclient *zclient)
{
  if (zclient_debug)
    zlog_debug ("zclient stopped");

  /* Stop threads. */
  THREAD_OFF(zclient->t_read);
  THREAD_OFF(zclient->t_connect);
  THREAD_OFF(zclient->t_write);

  if (zclient->qf)
    {
      /* remove from selection, if any, and forget all about it.
       *
       * If have no zclient->sock, should not have a qpd_file_fd either.
       *
       * May have a zclient->sock, but not have set up the qps_file.
       */
      if (zclient->sock < 0)
        qassert(qps_file_fd(zclient->qf) < 0) ;

      qps_remove_file(zclient->qf) ;
      qps_file_unset_fd(zclient->qf) ;
    } ;

  if (zclient->qtr)
    qtimer_unset(zclient->qtr);

  /* Reset streams. */
  stream_reset(zclient->ibuf);
  stream_reset(zclient->obuf);

  /* Empty the write buffer. */
  buffer_reset(zclient->wb);

  /* Close socket. */
  if (zclient->sock >= 0)
    {
      close (zclient->sock);
      zclient->sock = -1;
    }
}

void
zclient_reset (struct zclient *zclient)
{
  zclient_stop (zclient);
  zclient_init (zclient, zclient->redist_default);
}

#ifdef HAVE_TCP_ZEBRA

/* Make socket to zebra daemon. Return zebra socket. */
static int
zclient_socket(void)
{
  int sock;
  int ret;
  struct sockaddr_in serv;

  /* We should think about IPv6 connection. */
  sock = socket (AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return -1;

  /* Make server socket. */
  memset (&serv, 0, sizeof (struct sockaddr_in));
  serv.sin_family = AF_INET;
  serv.sin_port = htons (ZEBRA_PORT);
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
  serv.sin_len = sizeof (struct sockaddr_in);
#endif /* HAVE_STRUCT_SOCKADDR_IN_SIN_LEN */
  serv.sin_addr.s_addr = htonl (INADDR_LOOPBACK);

  /* Connect to zebra. */
  ret = connect (sock, (struct sockaddr *) &serv, sizeof (serv));
  if (ret < 0)
    {
      close (sock);
      return -1;
    }
  return sock;
}

#else

/* For sockaddr_un. */
#include <sys/un.h>

static int
zclient_socket_un (const char *path)
{
  int ret;
  int sock, len;
  struct sockaddr_un addr;

  sock = socket (AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0)
    return -1;

  /* Make server socket. */
  memset (&addr, 0, sizeof (struct sockaddr_un));
  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, path, strlen (path));
#ifdef HAVE_STRUCT_SOCKADDR_UN_SUN_LEN
  len = addr.sun_len = SUN_LEN(&addr);
#else
  len = sizeof (addr.sun_family) + strlen (addr.sun_path);
#endif /* HAVE_STRUCT_SOCKADDR_UN_SUN_LEN */

  ret = connect (sock, (struct sockaddr *) &addr, len);
  if (ret < 0)
    {
      close (sock);
      return -1;
    }
  return sock;
}

#endif /* HAVE_TCP_ZEBRA */

/**
 * Connect to zebra daemon.
 * @param zclient a pointer to zclient structure
 * @return socket fd just to make sure that connection established
 * @see zclient_init
 * @see zclient_new
 */
static int
zclient_socket_connect (struct zclient *zc)
{
#ifdef HAVE_TCP_ZEBRA
  zc->sock = zclient_socket ();
#else
  zc->sock = zclient_socket_un (zclient_serv_path ? zclient_serv_path
                                                  : ZEBRA_SERV_PATH);
#endif
  return zc->sock;
}

static int
zclient_failed(struct zclient *zclient)
{
  zclient->fail++;
  zclient_stop(zclient);
  zclient_event(ZCLIENT_CONNECT, zclient);
  return -1;
}

/* Write as much data as possible.
 * nexus version */
static void
zclient_flush_data_r(qps_file qf, void* file_info)
{
  struct zclient *zclient = file_info;

  qps_disable_modes(qf, qps_write_mbit);

  if (zclient->sock < 0)
    return;

  switch (buffer_flush_available(zclient->wb, zclient->sock))
    {
    case BUFFER_ERROR:
      zlog_warn("%s: buffer_flush_available failed on zclient fd %d, closing",
                __func__, zclient->sock);
      zclient_failed(zclient);
      break;
    case BUFFER_PENDING:
      qps_enable_mode(qf, qps_write_mnum, zclient_flush_data_r) ;
      break;
    case BUFFER_EMPTY:
      break;
    }
}

/* Write as much data as possible.
 * thread version */
static int
zclient_flush_data_t(struct thread *thread)
{
  struct zclient *zclient = THREAD_ARG(thread);

  zclient->t_write = NULL;
  if (zclient->sock < 0)
    return -1;
  switch (buffer_flush_available(zclient->wb, zclient->sock))
    {
    case BUFFER_ERROR:
      zlog_warn("%s: buffer_flush_available failed on zclient fd %d, closing",
      		__func__, zclient->sock);
      return zclient_failed(zclient);
      break;
    case BUFFER_PENDING:
      zclient->t_write = thread_add_write(master, zclient_flush_data_t,
					  zclient, zclient->sock);
      break;
    case BUFFER_EMPTY:
      break;
    }
  return 0;
}

int
zclient_send_message(struct zclient *zclient)
{
  if (zclient->sock < 0)
    return -1;
  switch (buffer_write(zclient->wb, zclient->sock, STREAM_DATA(zclient->obuf),
		       stream_get_endp(zclient->obuf)))
    {
    case BUFFER_ERROR:
      zlog_warn("%s: buffer_write failed to zclient fd %d, closing",
      		 __func__, zclient->sock);
      return zclient_failed(zclient);
      break;
    case BUFFER_EMPTY:
      if (zclient_nexus)
        qps_disable_modes(zclient->qf, qps_write_mbit);
      else
        THREAD_OFF(zclient->t_write);
      break;
    case BUFFER_PENDING:
      if (zclient_nexus)
        qps_enable_mode(zclient->qf, qps_write_mnum, zclient_flush_data_r) ;
      else
        THREAD_WRITE_ON(master, zclient->t_write,
                        zclient_flush_data_t, zclient, zclient->sock);
      break;
    }
  return 0;
}

void
zclient_create_header (struct stream *s, uint16_t command)
{
  /* length placeholder, caller can update */
  stream_putw (s, ZEBRA_HEADER_SIZE);
  stream_putc (s, ZEBRA_HEADER_MARKER);
  stream_putc (s, ZSERV_VERSION);
  stream_putw (s, command);
}

/* Send simple Zebra message. */
static int
zebra_message_send (struct zclient *zclient, int command)
{
  struct stream *s;

  /* Get zclient output buffer. */
  s = zclient->obuf;
  stream_reset (s);

  /* Send very simple command only Zebra message. */
  zclient_create_header (s, command);

  return zclient_send_message(zclient);
}

static int
zebra_hello_send (struct zclient *zclient)
{
  struct stream *s;

  if (zclient->redist_default)
    {
      s = zclient->obuf;
      stream_reset (s);

      zclient_create_header (s, ZEBRA_HELLO);
      stream_putc (s, zclient->redist_default);
      stream_putw_at (s, 0, stream_get_endp (s));
      return zclient_send_message(zclient);
    }

  return 0;
}

/* Make connection to zebra daemon. */
int
zclient_start (struct zclient *zclient)
{
  int i;

  if (zclient_debug)
    zlog_debug ("zclient_start is called");

  /* zclient is disabled. */
  if (! zclient->enable)
    return 0;

  /* If already connected to the zebra. */
  if (zclient->sock >= 0)
    return 0;

  /* Check connect thread. */
  if (zclient->t_connect)
    return 0;

  /* Check timer */
  if (qtimer_is_active(zclient->qtr))
    return 0;

  /* Make socket. */
  if (zclient_socket_connect(zclient) < 0)
    {
      if (zclient_debug)
	zlog_debug ("zclient connection fail");
      zclient->fail++;
      zclient_event (ZCLIENT_CONNECT, zclient);
      return -1;
    }

  if (zclient_nexus)
    qps_add_file(zclient_nexus->selection, zclient->qf, zclient->sock, zclient);

  if (set_nonblocking(zclient->sock) < 0)
    zlog_warn("%s: set_nonblocking(%d) failed", __func__, zclient->sock);

  /* Clear fail count. */
  zclient->fail = 0;
  if (zclient_debug)
    zlog_debug ("zclient connect success with socket [%d]", zclient->sock);

  /* Create read thread. */
  zclient_event (ZCLIENT_READ, zclient);

  zebra_hello_send (zclient);

  /* We need router-id information. */
  zebra_message_send (zclient, ZEBRA_ROUTER_ID_ADD);

  /* We need interface information. */
  zebra_message_send (zclient, ZEBRA_INTERFACE_ADD);

  /* Flush all redistribute request. */
  for (i = 0; i < ZEBRA_ROUTE_MAX; i++)
    if (i != zclient->redist_default && zclient->redist[i])
      zebra_redistribute_send (ZEBRA_REDISTRIBUTE_ADD, zclient, i);

  /* If default information is needed. */
  if (zclient->default_information)
    zebra_message_send (zclient, ZEBRA_REDISTRIBUTE_DEFAULT_ADD);

  return 0;
}

/* This function is a wrapper function for calling zclient_start from
   qtimer. */
static void
zclient_connect_r (qtimer qtr, void* timer_info, qtime_t when)
{
  struct zclient *zclient = timer_info;

  qtimer_unset(qtr);

  if (zclient_debug)
    zlog_debug ("zclient_connect is called");

  zclient_start (zclient);
}

/* This function is a wrapper function for calling zclient_start from
   timer or event thread. */
static int
zclient_connect_t (struct thread *t)
{
  struct zclient *zclient;

  zclient = THREAD_ARG (t);
  zclient->t_connect = NULL;

  if (zclient_debug)
    zlog_debug ("zclient_connect is called");

  return zclient_start (zclient);
}

/* Connect to zebra for nexthop lookup.
 * thread version */
static int
zlookup_connect_t (struct thread *t)
{
  struct zclient *zlookup;

  zlookup = THREAD_ARG (t);
  zlookup->t_connect = NULL;

  if (zlookup->sock < 0)
    zclient_socket_connect(zlookup) ;

  return (zlookup->sock >= 0) ? 0 : -1 ;
}

/* Connect to zebra for nexthop lookup.
 * nexus version */
static void
zlookup_connect_r (qtimer qtr, void* timer_info, qtime_t when)
{
  struct zclient *zlookup = timer_info;

  qtimer_unset(qtr);

  if (zlookup->sock < 0)
    zclient_socket_connect(zlookup) ;
}

 /*
  * "xdr_encode"-like interface that allows daemon (client) to send
  * a message to zebra server for a route that needs to be
  * added/deleted to the kernel. Info about the route is specified
  * by the caller in a struct zapi_ipv4. zapi_ipv4_read() then writes
  * the info down the zclient socket using the stream_* functions.
  *
  * The corresponding read ("xdr_decode") function on the server
  * side is zread_ipv4_add()/zread_ipv4_delete().
  *
  *  0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
  * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  * |            Length (2)         |    Command    | Route Type    |
  * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  * | ZEBRA Flags   | Message Flags |             SAFI              |
  * +---------------+---------------+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  * | Prefix length |
  * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  * | Destination IPv4 Prefix for route                             |
  * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  * | Nexthop count |
  * +-+-+-+-+-+-+-+-+
  *
  *
  * A number of IPv4 nexthop(s) or nexthop interface index(es) are then
  * described, as per the Nexthop count. Each nexthop described as:
  *
  * +-+-+-+-+-+-+-+-+
  * | Nexthop Type  |  Set to one of ZEBRA_NEXTHOP_*
  * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  * |       IPv4 Nexthop address or Interface Index number          |
  * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  *
  * Alternatively, if the flags field has ZEBRA_FLAG_BLACKHOLE or
  * ZEBRA_FLAG_REJECT is set then Nexthop count is set to 1, then _no_
  * nexthop information is provided, and the message describes a prefix
  * to blackhole or reject route.
  *
  * If ZAPI_MESSAGE_DISTANCE is set, the distance value is written as a 1
  * byte value.
  *
  * If ZAPI_MESSAGE_METRIC is set, the metric value is written as an 8
  * byte value.
  *
  * XXX: No attention paid to alignment.
  */
int
zapi_ipv4_route (u_char cmd, struct zclient *zclient, struct prefix_ipv4 *p,
                 struct zapi_ipv4 *api)
{
  int i;
  int psize;
  struct stream *s;

  /* Reset stream. */
  s = zclient->obuf;
  stream_reset (s);

  zclient_create_header (s, cmd);

  /* Put type and nexthop. */
  stream_putc (s, api->type);
  stream_putc (s, api->flags);
  stream_putc (s, api->message);
  stream_putw (s, api->safi);

  /* Put prefix information. */
  psize = PSIZE (p->prefixlen);
  stream_putc (s, p->prefixlen);
  stream_put (s, (u_char *) & p->prefix, psize);

  /* Nexthop, ifindex, distance and metric information. */
  if (CHECK_FLAG (api->message, ZAPI_MESSAGE_NEXTHOP))
    {
      if (CHECK_FLAG (api->flags, ZEBRA_FLAG_BLACKHOLE))
        {
          stream_putc (s, 1);
          stream_putc (s, ZEBRA_NEXTHOP_BLACKHOLE);
          /* XXX assert(api->nexthop_num == 0); */
          /* XXX assert(api->ifindex_num == 0); */
        }
      else
        stream_putc (s, api->nexthop_num + api->ifindex_num);

      for (i = 0; i < api->nexthop_num; i++)
        {
          stream_putc (s, ZEBRA_NEXTHOP_IPV4);
          stream_put_in_addr (s, api->nexthop[i]);
        }
      for (i = 0; i < api->ifindex_num; i++)
        {
          stream_putc (s, ZEBRA_NEXTHOP_IFINDEX);
          stream_putl (s, api->ifindex[i]);
        }
    }

  if (CHECK_FLAG (api->message, ZAPI_MESSAGE_DISTANCE))
    stream_putc (s, api->distance);
  if (CHECK_FLAG (api->message, ZAPI_MESSAGE_METRIC))
    stream_putl (s, api->metric);

  /* Put length at the first point of the stream. */
  stream_putw_at (s, 0, stream_get_endp (s));

  return zclient_send_message(zclient);
}

#ifdef HAVE_IPV6
int
zapi_ipv6_route (u_char cmd, struct zclient *zclient, struct prefix_ipv6 *p,
	       struct zapi_ipv6 *api)
{
  int i;
  int psize;
  struct stream *s;

  /* Reset stream. */
  s = zclient->obuf;
  stream_reset (s);

  zclient_create_header (s, cmd);

  /* Put type and nexthop. */
  stream_putc (s, api->type);
  stream_putc (s, api->flags);
  stream_putc (s, api->message);
  stream_putw (s, api->safi);

  /* Put prefix information. */
  psize = PSIZE (p->prefixlen);
  stream_putc (s, p->prefixlen);
  stream_put (s, (u_char *)&p->prefix, psize);

  /* Nexthop, ifindex, distance and metric information. */
  if (CHECK_FLAG (api->message, ZAPI_MESSAGE_NEXTHOP))
    {
      stream_putc (s, api->nexthop_num + api->ifindex_num);

      for (i = 0; i < api->nexthop_num; i++)
	{
	  stream_putc (s, ZEBRA_NEXTHOP_IPV6);
	  stream_put (s, (u_char *)api->nexthop[i], 16);
	}
      for (i = 0; i < api->ifindex_num; i++)
	{
	  stream_putc (s, ZEBRA_NEXTHOP_IFINDEX);
	  stream_putl (s, api->ifindex[i]);
	}
    }

  if (CHECK_FLAG (api->message, ZAPI_MESSAGE_DISTANCE))
    stream_putc (s, api->distance);
  if (CHECK_FLAG (api->message, ZAPI_MESSAGE_METRIC))
    stream_putl (s, api->metric);

  /* Put length at the first point of the stream. */
  stream_putw_at (s, 0, stream_get_endp (s));

  return zclient_send_message(zclient);
}
#endif /* HAVE_IPV6 */

/*
 * send a ZEBRA_REDISTRIBUTE_ADD or ZEBRA_REDISTRIBUTE_DELETE
 * for the route type (ZEBRA_ROUTE_KERNEL etc.). The zebra server will
 * then set/unset redist[type] in the client handle (a struct zserv) for the
 * sending client
 */
int
zebra_redistribute_send (int command, struct zclient *zclient, int type)
{
  struct stream *s;

  s = zclient->obuf;
  stream_reset(s);

  zclient_create_header (s, command);
  stream_putc (s, type);

  stream_putw_at (s, 0, stream_get_endp (s));

  return zclient_send_message(zclient);
}

/* Router-id update from zebra daemon. */
void
zebra_router_id_update_read (struct stream *s, struct prefix *rid)
{
  int plen;

  /* Fetch interface address. */
  rid->family = stream_getc (s);

  plen = prefix_blen (rid);
  stream_get (&rid->u.prefix, s, plen);
  rid->prefixlen = stream_getc (s);
}

/* Interface addition from zebra daemon. */
/*
 * The format of the message sent with type ZEBRA_INTERFACE_ADD or
 * ZEBRA_INTERFACE_DELETE from zebra to the client is:
 *     0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+
 * |   type        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |  ifname                                                       |
 * |                                                               |
 * |                                                               |
 * |                                                               |
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         ifindex                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         if_flags                                              |
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         metric                                                |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         ifmtu                                                 |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         ifmtu6                                                |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         bandwidth                                             |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         sockaddr_dl                                           |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */

struct interface *
zebra_interface_add_read (struct stream *s)
{
  struct interface *ifp;
  char ifname_tmp[INTERFACE_NAMSIZ];

  /* Read interface name. */
  stream_get (ifname_tmp, s, INTERFACE_NAMSIZ);

  /* Lookup/create interface by name. */
  ifp = if_get_by_name_len (ifname_tmp, strnlen(ifname_tmp, INTERFACE_NAMSIZ));

  /* Read interface's index. */
  ifp->ifindex = stream_getl (s);

  /* Read interface's value. */
  ifp->status = stream_getc (s);
  ifp->flags = stream_getq (s);
  ifp->metric = stream_getl (s);
  ifp->mtu = stream_getl (s);
  ifp->mtu6 = stream_getl (s);
  ifp->bandwidth = stream_getl (s);
#ifdef HAVE_STRUCT_SOCKADDR_DL
  stream_get (&ifp->sdl, s, sizeof (ifp->sdl));
#else
  ifp->hw_addr_len = stream_getl (s);
  if (ifp->hw_addr_len)
    stream_get (ifp->hw_addr, s, ifp->hw_addr_len);
#endif /* HAVE_STRUCT_SOCKADDR_DL */

  return ifp;
}

/*
 * Read interface up/down msg (ZEBRA_INTERFACE_UP/ZEBRA_INTERFACE_DOWN)
 * from zebra server.  The format of this message is the same as
 * that sent for ZEBRA_INTERFACE_ADD/ZEBRA_INTERFACE_DELETE (see
 * comments for zebra_interface_add_read), except that no sockaddr_dl
 * is sent at the tail of the message.
 */
struct interface *
zebra_interface_state_read (struct stream *s)
{
  struct interface *ifp;
  char ifname_tmp[INTERFACE_NAMSIZ];

  /* Read interface name. */
  stream_get (ifname_tmp, s, INTERFACE_NAMSIZ);

  /* Lookup this by interface index. */
  ifp = if_lookup_by_name_len (ifname_tmp,
			       strnlen(ifname_tmp, INTERFACE_NAMSIZ));

  /* If such interface does not exist, indicate an error */
  if (! ifp)
     return NULL;

  /* Read interface's index. */
  ifp->ifindex = stream_getl (s);

  /* Read interface's value. */
  ifp->status = stream_getc (s);
  ifp->flags = stream_getq (s);
  ifp->metric = stream_getl (s);
  ifp->mtu = stream_getl (s);
  ifp->mtu6 = stream_getl (s);
  ifp->bandwidth = stream_getl (s);

  return ifp;
}

/*
 * format of message for address additon is:
 *    0
 *  0 1 2 3 4 5 6 7
 * +-+-+-+-+-+-+-+-+
 * |   type        |  ZEBRA_INTERFACE_ADDRESS_ADD or
 * +-+-+-+-+-+-+-+-+  ZEBRA_INTERFACE_ADDRES_DELETE
 * |               |
 * +               +
 * |   ifindex     |
 * +               +
 * |               |
 * +               +
 * |               |
 * +-+-+-+-+-+-+-+-+
 * |   ifc_flags   |  flags for connected address
 * +-+-+-+-+-+-+-+-+
 * |  addr_family  |
 * +-+-+-+-+-+-+-+-+
 * |    addr...    |
 * :               :
 * |               |
 * +-+-+-+-+-+-+-+-+
 * |    addr_len   |  len of addr. E.g., addr_len = 4 for ipv4 addrs.
 * +-+-+-+-+-+-+-+-+
 * |     daddr..   |
 * :               :
 * |               |
 * +-+-+-+-+-+-+-+-+
 *
 */

void
zebra_interface_if_set_value (struct stream *s, struct interface *ifp)
{
  /* Read interface's index. */
  ifp->ifindex = stream_getl (s);
  ifp->status = stream_getc (s);

  /* Read interface's value. */
  ifp->flags = stream_getq (s);
  ifp->metric = stream_getl (s);
  ifp->mtu = stream_getl (s);
  ifp->mtu6 = stream_getl (s);
  ifp->bandwidth = stream_getl (s);
}

static int
memconstant(const void *s, int c, size_t n)
{
  const u_char *p = s;

  while (n-- > 0)
    if (*p++ != c)
      return 0;
  return 1;
}

struct connected *
zebra_interface_address_read (int type, struct stream *s)
{
  unsigned int ifindex;
  struct interface *ifp;
  struct connected *ifc;
  struct prefix p, d;
  int family;
  int plen;
  u_char ifc_flags;

  memset (&p, 0, sizeof(p));
  memset (&d, 0, sizeof(d));

  /* Get interface index. */
  ifindex = stream_getl (s);

  /* Lookup index. */
  ifp = if_lookup_by_index (ifindex);
  if (ifp == NULL)
    {
      zlog_warn ("zebra_interface_address_read(%s): "
                 "Can't find interface by ifindex: %d ",
                 (type == ZEBRA_INTERFACE_ADDRESS_ADD? "ADD" : "DELETE"),
                 ifindex);
      return NULL;
    }

  /* Fetch flag. */
  ifc_flags = stream_getc (s);

  /* Fetch interface address. */
  family = p.family = stream_getc (s);

  plen = prefix_blen (&p);
  stream_get (&p.u.prefix, s, plen);
  p.prefixlen = stream_getc (s);

  /* Fetch destination address. */
  stream_get (&d.u.prefix, s, plen);
  d.family = family;

  if (type == ZEBRA_INTERFACE_ADDRESS_ADD)
    {
       /* N.B. NULL destination pointers are encoded as all zeroes */
       ifc = connected_add_by_prefix(ifp, &p,(memconstant(&d.u.prefix,0,plen) ?
					      NULL : &d));
       if (ifc != NULL)
	 {
	   ifc->flags = ifc_flags;
	   if (ifc->destination)
	     ifc->destination->prefixlen = ifc->address->prefixlen;
	 }
    }
  else
    {
      assert (type == ZEBRA_INTERFACE_ADDRESS_DELETE);
      ifc = connected_delete_by_prefix(ifp, &p);
    }

  return ifc;
}

/* nexus: Zebra client message read function. */
static void
zclient_read_r (qps_file qf, void* file_info)
{
  struct zclient *zclient = file_info;
  qps_disable_modes(qf, qps_read_mbit);
  zclient_read(zclient);
}

/* thread: Zebra client message read function. */
static int
zclient_read_t (struct thread *thread)
{
  struct zclient *zclient = THREAD_ARG (thread);
  zclient->t_read = NULL;
  return zclient_read(zclient);
}

/* Zebra client message read function. */
static int
zclient_read (struct zclient *zclient)
{
  size_t already;
  uint16_t length, command;
  uint8_t marker, version;

  /* Read zebra header (if we don't have it already). */
  if ((already = stream_get_endp(zclient->ibuf)) < ZEBRA_HEADER_SIZE)
    {
      ssize_t nbyte;
      if (((nbyte = stream_read_try(zclient->ibuf, zclient->sock,
				     ZEBRA_HEADER_SIZE-already)) == 0) ||
	  (nbyte == -1))
	{
	  if (zclient_debug)
	   zlog_debug ("zclient connection closed socket [%d].", zclient->sock);
	  return zclient_failed(zclient);
	}
      if (nbyte != (ssize_t)(ZEBRA_HEADER_SIZE-already))
	{
	  /* Try again later. */
	  zclient_event (ZCLIENT_READ, zclient);
	  return 0;
	}
      already = ZEBRA_HEADER_SIZE;
    }

  /* Reset to read from the beginning of the incoming packet. */
  stream_set_getp(zclient->ibuf, 0);

  /* Fetch header values. */
  length = stream_getw (zclient->ibuf);
  marker = stream_getc (zclient->ibuf);
  version = stream_getc (zclient->ibuf);
  command = stream_getw (zclient->ibuf);

  if (marker != ZEBRA_HEADER_MARKER || version != ZSERV_VERSION)
    {
      zlog_err("%s: socket %d version mismatch, marker %d, version %d",
               __func__, zclient->sock, marker, version);
      return zclient_failed(zclient);
    }

  if (length < ZEBRA_HEADER_SIZE)
    {
      zlog_err("%s: socket %d message length %u is less than %d ",
	       __func__, zclient->sock, length, ZEBRA_HEADER_SIZE);
      return zclient_failed(zclient);
    }

  /* Length check. */
  if (length > stream_get_size(zclient->ibuf))
    {
      struct stream *ns;
      zlog_warn("%s: message size %u exceeds buffer size %zu, expanding...",
	                     __func__, length, stream_get_size(zclient->ibuf));
      ns = stream_new(length);
      stream_copy(ns, zclient->ibuf);
      stream_free (zclient->ibuf);
      zclient->ibuf = ns;
    }

  /* Read rest of zebra packet. */
  if (already < length)
    {
      ssize_t nbyte;
      if (((nbyte = stream_read_try(zclient->ibuf, zclient->sock,
				     length-already)) == 0) ||
	  (nbyte == -1))
	{
	  if (zclient_debug)
	    zlog_debug("zclient connection closed socket [%d].", zclient->sock);
	  return zclient_failed(zclient);
	}
      if (nbyte != (ssize_t)(length-already))
	{
	  /* Try again later. */
	  zclient_event (ZCLIENT_READ, zclient);
	  return 0;
	}
    }

  length -= ZEBRA_HEADER_SIZE;

  if (zclient_debug)
    zlog_debug("zclient 0x%p command 0x%x \n", zclient, command);

  switch (command)
    {
    case ZEBRA_ROUTER_ID_UPDATE:
      if (zclient->router_id_update)
	(*zclient->router_id_update) (command, zclient, length);
      break;
    case ZEBRA_INTERFACE_ADD:
      if (zclient->interface_add)
	(*zclient->interface_add) (command, zclient, length);
      break;
    case ZEBRA_INTERFACE_DELETE:
      if (zclient->interface_delete)
	(*zclient->interface_delete) (command, zclient, length);
      break;
    case ZEBRA_INTERFACE_ADDRESS_ADD:
      if (zclient->interface_address_add)
	(*zclient->interface_address_add) (command, zclient, length);
      break;
    case ZEBRA_INTERFACE_ADDRESS_DELETE:
      if (zclient->interface_address_delete)
	(*zclient->interface_address_delete) (command, zclient, length);
      break;
    case ZEBRA_INTERFACE_UP:
      if (zclient->interface_up)
	(*zclient->interface_up) (command, zclient, length);
      break;
    case ZEBRA_INTERFACE_DOWN:
      if (zclient->interface_down)
	(*zclient->interface_down) (command, zclient, length);
      break;
    case ZEBRA_IPV4_ROUTE_ADD:
      if (zclient->ipv4_route_add)
	(*zclient->ipv4_route_add) (command, zclient, length);
      break;
    case ZEBRA_IPV4_ROUTE_DELETE:
      if (zclient->ipv4_route_delete)
	(*zclient->ipv4_route_delete) (command, zclient, length);
      break;
    case ZEBRA_IPV6_ROUTE_ADD:
      if (zclient->ipv6_route_add)
	(*zclient->ipv6_route_add) (command, zclient, length);
      break;
    case ZEBRA_IPV6_ROUTE_DELETE:
      if (zclient->ipv6_route_delete)
	(*zclient->ipv6_route_delete) (command, zclient, length);
      break;
    default:
      break;
    }

  if (zclient->sock < 0)
    /* Connection was closed during packet processing. */
    return -1;

  /* Register read thread. */
  stream_reset(zclient->ibuf);
  zclient_event (ZCLIENT_READ, zclient);

  return 0;
}

void
zclient_redistribute (int command, struct zclient *zclient, int type)
{

  if (command == ZEBRA_REDISTRIBUTE_ADD)
    {
      if (zclient->redist[type])
         return;
      zclient->redist[type] = 1;
    }
  else
    {
      if (!zclient->redist[type])
         return;
      zclient->redist[type] = 0;
    }

  if (zclient->sock > 0)
    zebra_redistribute_send (command, zclient, type);
}


void
zclient_redistribute_default (int command, struct zclient *zclient)
{

  if (command == ZEBRA_REDISTRIBUTE_DEFAULT_ADD)
    {
      if (zclient->default_information)
        return;
      zclient->default_information = 1;
    }
  else
    {
      if (!zclient->default_information)
        return;
      zclient->default_information = 0;
    }

  if (zclient->sock > 0)
    zebra_message_send (zclient, command);
}

/* Arm event. */
static void
zclient_event (enum event event, struct zclient *zclient)
{
  if (zclient_nexus)
    zclient_event_r(event, zclient);
  else
    zclient_event_t(event, zclient);
}

/* Arm event.
 * nexus version */
static void
zclient_event_r (enum event event, struct zclient *zclient)
{
  switch (event)
    {
    case ZLOOKUP_SCHEDULE:
      if (!qtimer_is_active(zclient->qtr))
        qtimer_set(zclient->qtr, qt_get_monotonic(), zlookup_connect_r) ;
      break;
    case ZCLIENT_SCHEDULE:
      if (!qtimer_is_active(zclient->qtr))
        qtimer_set(zclient->qtr, qt_get_monotonic(), zclient_connect_r) ;
      break;
    case ZCLIENT_CONNECT:
      if (zclient->fail >= 10)
	return;
      if (zclient_debug)
	zlog_debug ("zclient connect schedule interval is %d",
		   zclient->fail < 3 ? 10 : 60);
      if (!qtimer_is_active(zclient->qtr))
        qtimer_set(zclient->qtr,
          qt_add_monotonic(QTIME(zclient->fail < 3 ? 10 : 60)), zclient_connect_r) ;
      break;
    case ZCLIENT_READ:
         qps_enable_mode(zclient->qf, qps_read_mnum, zclient_read_r) ;
      break;
    }
}

/* Arm event.
 * thread version */
static void
zclient_event_t (enum event event, struct zclient *zclient)
{
  switch (event)
    {
    case ZLOOKUP_SCHEDULE:
      if (! zclient->t_connect)
        zclient->t_connect =
            thread_add_event (master, zlookup_connect_t, zclient, 0);
    break;
    case ZCLIENT_SCHEDULE:
      if (! zclient->t_connect)
        zclient->t_connect =
          thread_add_event (master, zclient_connect_t, zclient, 0);
      break;
    case ZCLIENT_CONNECT:
      if (zclient->fail >= 10)
        return;
      if (zclient_debug)
        zlog_debug ("zclient connect schedule interval is %d",
                   zclient->fail < 3 ? 10 : 60);
      if (! zclient->t_connect)
        zclient->t_connect =
          thread_add_timer (master, zclient_connect_t, zclient,
                            zclient->fail < 3 ? 10 : 60);
      break;
    case ZCLIENT_READ:
      zclient->t_read =
          thread_add_read (master, zclient_read_t, zclient, zclient->sock);
      break;
    }
}

void
zclient_serv_path_set (char *path)
{
  struct stat sb;

  /* reset */
  zclient_serv_path = NULL;

  /* test if `path' is socket. don't set it otherwise. */
  if (stat(path, &sb) == -1)
    {
      zlog_warn ("%s: zebra socket `%s' does not exist", __func__, path);
      return;
    }

  if ((sb.st_mode & S_IFMT) != S_IFSOCK)
    {
      zlog_warn ("%s: `%s' is not unix socket, sir", __func__, path);
      return;
    }

  /* it seems that path is unix socket */
  zclient_serv_path = path;
}

