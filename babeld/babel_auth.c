/*
Copyright 2012 by Denis Ovsienko

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <zebra.h>
#include "log.h"
#include "memory.h"
#include "stream.h"
#include "linklist.h"
#include "cryptohash.h"
#include "keychain.h"

#include "babeld/babel_auth.h"
#include "babeld/babel_interface.h"
#include "babeld/util.h"
#include "babeld/message.h"

#define BABEL_TS_BASE_ZERO        0
#define BABEL_TS_BASE_UNIX        1
/* Using "UNIX timestamp" as default TS base in this implementation will remain
 * reliable as long as return value of quagga_time() remains non-decreasing. */
#define BABEL_DEFAULT_TS_BASE     BABEL_TS_BASE_UNIX
/* Low default timeout allows for naive neighbors with "zero" TS base to reboot
 * without getting locked out for a long time. */
#define BABEL_DEFAULT_ANM_TIMEOUT 300

/* authentic neighbors memory */
struct babel_anm_item
{
  struct in6_addr address;
  struct interface *ifp;
  time_t last_recv;
  u_int16_t last_pc;
  u_int32_t last_ts;
};

/* effective security association */
struct babel_esa_item
{
  unsigned sort_order_major;
  unsigned sort_order_minor;
  unsigned hash_algo;
  u_int16_t key_id;
  size_t key_len;
  u_int8_t * key_secret;
};

/* local routing process variables */
static unsigned char ts_base;
static u_int32_t anm_timeout;
static struct list *anmlist;
static struct babel_auth_stats stats;

static const struct message ts_base_cli_str[] =
{
  { BABEL_TS_BASE_ZERO, "zero"      },
  { BABEL_TS_BASE_UNIX, "unixtime"  },
};
static const size_t ts_base_cli_str_max = sizeof (ts_base_cli_str) / sizeof (struct message);

static const struct message ts_base_str[] =
{
  { BABEL_TS_BASE_ZERO, "NVRAM-less PC wrap counter"  },
  { BABEL_TS_BASE_UNIX, "UNIX time w/PC wrap counter" },
};
static const size_t ts_base_str_max = sizeof (ts_base_str) / sizeof (struct message);


/* List hook function to deallocate an ANM record. */
static void
babel_anm_free (void * node)
{
  XFREE (MTYPE_BABEL_AUTH, node);
}

#ifdef HAVE_LIBGCRYPT
/* Return an ANM record addressed with the given (source address, interface)
 * pair, or NULL, if it is missing. */
static struct babel_anm_item *
babel_anm_lookup (const struct in6_addr *address, const struct interface *ifp)
{
  struct listnode *node;
  struct babel_anm_item *anm;

  for (ALL_LIST_ELEMENTS_RO (anmlist, node, anm))
    if (IPV6_ADDR_SAME (&anm->address, address) && anm->ifp == ifp)
      return anm;
  return NULL;
}

/* Return an ANM record addressed with the given (source address, interface)
 * pair. Create a new record, if a need is. */
static struct babel_anm_item *
babel_anm_get (const struct in6_addr *address, struct interface *ifp)
{
  struct babel_anm_item *anm;
  char buffer[INET6_ADDRSTRLEN];

  if (! (anm = babel_anm_lookup (address, ifp)))
  {
    anm = XCALLOC (MTYPE_BABEL_AUTH, sizeof (struct babel_anm_item));
    IPV6_ADDR_COPY (&anm->address, address);
    anm->ifp = ifp;
    listnode_add (anmlist, anm);
    if (UNLIKELY (debug & BABEL_DEBUG_AUTH))
    {
      inet_ntop (AF_INET6, &anm->address, buffer, INET6_ADDRSTRLEN);
      zlog_debug ("%s: adding memory record for %s", __func__, buffer);
    }
  }
  return anm;
}

/* This periodic timer flushes expired records from authentic neighbors memory. */
int
babel_auth_do_housekeeping (struct thread *thread)
{
  struct listnode *node, *nextnode;
  struct babel_anm_item *anm;
  time_t now = quagga_time (NULL);
  char buffer[INET6_ADDRSTRLEN];

  for (ALL_LIST_ELEMENTS (anmlist, node, nextnode, anm))
    if (anm->last_recv + anm_timeout < now)
    {
      if (UNLIKELY (debug & BABEL_DEBUG_AUTH))
      {
        inet_ntop (AF_INET6, &anm->address, buffer, INET6_ADDRSTRLEN);
        zlog_debug ("%s: memory record for %s has expired", __func__, buffer);
      }
      listnode_delete (anmlist, anm);
      XFREE (MTYPE_BABEL_AUTH, anm);
    }
  schedule_auth_housekeeping();
  return 0;
}

/* Deep copy function to allocate an ESA record. */
static struct babel_esa_item *
babel_esa_item_alloc
(
  const u_int16_t hash_algo,
  const u_int16_t key_id,
  const size_t key_len,
  const u_int8_t * key_secret
)
{
  struct babel_esa_item *copy = XCALLOC (MTYPE_BABEL_AUTH, sizeof (struct babel_esa_item));
  copy->hash_algo = hash_algo;
  copy->key_id = key_id;
  copy->key_len = key_len;
  copy->key_secret = XMALLOC (MTYPE_BABEL_AUTH, key_len);
  memcpy (copy->key_secret, key_secret, key_len);
  return copy;
}

/* List hook function to deallocate an ESA record. */
static void
babel_esa_item_free (void * esa_key)
{
  XFREE (MTYPE_BABEL_AUTH, ((struct babel_esa_item *) esa_key)->key_secret);
  XFREE (MTYPE_BABEL_AUTH, esa_key);
}

/* List hook function to compare two ESA record. */
static int
babel_esa_item_cmp (struct babel_esa_item *val1, struct babel_esa_item *val2)
{
  if (val1->sort_order_major < val2->sort_order_major)
    return -1;
  if (val1->sort_order_major > val2->sort_order_major)
    return 1;
  if (val1->sort_order_minor < val2->sort_order_minor)
    return -1;
  if (val1->sort_order_minor > val2->sort_order_minor)
    return 1;
  return 0;
}

/* Return 1, if the list contains a ESA record with the same attributes. */
static unsigned char
babel_esa_item_exists
(
  const struct list *esalist,
  const unsigned new_hash_algo,
  const u_int16_t new_key_id,
  const size_t new_key_len,
  const u_int8_t * new_key_secret
)
{
  struct listnode *node;
  struct babel_esa_item *esa;

  for (ALL_LIST_ELEMENTS_RO (esalist, node, esa))
    if
    (
      esa->hash_algo == new_hash_algo &&
      esa->key_id == new_key_id &&
      esa->key_len == new_key_len &&
      ! memcmp (esa->key_secret, new_key_secret, new_key_len)
    )
      return 1;
  return 0;
}

/* Build and return a list of ESAs from a given list of CSAs, a time reference
 * and a filter function. The latter is typically keys_valid_for_send() or
 * keys_valid_for_accept(). Take care of suppressing full ESA duplicates.
 * Calling function is assumed to take care of deallocating the result with
 * list_delete(). */
static struct list *
babel_esalist_derive
(
  const struct list * csalist,
  const time_t now,
  struct list * (*keychain_filter_func) (const struct keychain *, const time_t)
)
{
  struct list *all_esas, *filtered_keys;
  struct listnode *node1, *node2;
  struct babel_csa_item *csa;
  struct keychain *keychain;
  struct key *key;
  unsigned csa_counter = 0;

  all_esas = list_new();
  all_esas->del = babel_esa_item_free;
  all_esas->cmp = (int (*) (void *, void *)) babel_esa_item_cmp;
  for (ALL_LIST_ELEMENTS_RO (csalist, node1, csa))
  {
    unsigned key_counter = 0;
    keychain = keychain_lookup (csa->keychain_name);
    if (! keychain)
    {
      debugf (BABEL_DEBUG_AUTH, "%s: keychain '%s' configured for %s does not exist", __func__,
              csa->keychain_name, LOOKUP (hash_algo_str, csa->hash_algo));
      continue;
    }
    debugf (BABEL_DEBUG_AUTH, "%s: found keychain '%s' with %u key(s) for %s", __func__,
            csa->keychain_name, listcount (keychain->key), LOOKUP (hash_algo_str, csa->hash_algo));
    filtered_keys = keychain_filter_func (keychain, now);
    for (ALL_LIST_ELEMENTS_RO (filtered_keys, node2, key))
    {
      struct babel_esa_item *esa;
      if (babel_esa_item_exists (all_esas, csa->hash_algo, key->index % (UINT16_MAX + 1),
          strlen (key->string), (u_int8_t *) key->string))
      {
        debugf (BABEL_DEBUG_AUTH, "%s: KeyID %u is a full duplicate of another key", __func__,
                key->index % (UINT16_MAX + 1));
        continue;
      }
      esa = babel_esa_item_alloc (csa->hash_algo, key->index % (UINT16_MAX + 1),
                                  strlen (key->string), (u_int8_t *)key->string);
      /* The all_esas list will have first keys of all CSAs in the order of CSAs,
       * then all second keys in the same order and so on. */
      esa->sort_order_major = key_counter;
      esa->sort_order_minor = csa_counter;
      listnode_add_sort (all_esas, esa);
      debugf (BABEL_DEBUG_AUTH, "%s: using KeyID %u with sort order %u major %u minor", __func__,
              esa->key_id, key_counter, csa_counter);
      key_counter++;
    }
    list_delete (filtered_keys);
    csa_counter++;
  }
  return all_esas;
}

/* Return "stream getp" coordinate of PC followed by TS, if the first TS/PC TLV
 * of the given packet exists and passes a constraint check against stored TS/PC
 * values for the address of packet sender. Return -1 otherwise and update two
 * pools of stats counters. */
static int
babel_auth_check_tspc (struct babel_auth_stats *if_stats, struct stream *packet,
                       const u_int16_t stor_pc, const u_int32_t stor_ts)
{
  int ret = -1;
  u_int8_t tlv_type, tlv_length;
  u_int16_t tlv_pc;
  u_int32_t tlv_ts;

  stream_set_getp (packet, 4);
  while (STREAM_READABLE (packet))
  {
    tlv_type = stream_getc (packet);
    if (tlv_type == MESSAGE_PAD1)
      continue;
    tlv_length = stream_getc (packet);
    if (tlv_type != MESSAGE_TSPC)
    {
      stream_forward_getp (packet, tlv_length);
      continue;
    }
    /* TS/PC TLV */
    tlv_pc = stream_getw (packet);
    tlv_ts = stream_getl (packet);
    if (tlv_ts > stor_ts || (tlv_ts == stor_ts && tlv_pc > stor_pc))
      ret = stream_get_getp (packet) - 6;
    else
    {
      stats.auth_recv_ng_tspc++;
      if_stats->auth_recv_ng_tspc++;
    }
    debugf (BABEL_DEBUG_AUTH, "%s: received TS/PC is (%u/%u), stored is (%u/%u), check %s", __func__,
            tlv_ts, tlv_pc, stor_ts, stor_pc, ret == -1 ? "failed" : "OK");
    /* only the 1st TLV matters */
    return ret;
  }
  stats.auth_recv_ng_no_tspc++;
  if_stats->auth_recv_ng_no_tspc++;
  debugf (BABEL_DEBUG_AUTH, "%s: no TS/PC TLV in the packet, check failed", __func__);
  return -1;
}

/* Make a copy of input packet, pad its HMAC TLVs and return the padded copy. */
static struct stream *
babel_auth_pad_packet (struct stream *packet, const unsigned char *addr6)
{
  struct stream *padded;
  u_int8_t tlv_type, tlv_length;

  padded = stream_dup (packet);
  stream_reset (padded);
  /* packet header is left unchanged */
  stream_forward_endp (padded, 4);
  stream_set_getp (packet, 4);
  while (STREAM_READABLE (packet))
  {
    tlv_type = stream_getc (packet);
    stream_forward_endp (padded, 1);
    if (tlv_type == MESSAGE_PAD1)
      continue;
    tlv_length = stream_getc (packet);
    stream_forward_endp (padded, 1);
    if (tlv_type != MESSAGE_HMAC)
      stream_forward_endp (padded, tlv_length);
    else
    {
      stream_forward_endp (padded, 2);
      debugf (BABEL_DEBUG_AUTH, "%s: padding %uB of digest at offset %zu", __func__,
              tlv_length - 2, stream_get_endp (padded));
      stream_put (padded, addr6, IPV6_MAX_BYTELEN);
      stream_put (padded, NULL, tlv_length - IPV6_MAX_BYTELEN - 2);
    }
    stream_forward_getp (packet, tlv_length);
  }
  assert (stream_get_endp (packet) == stream_get_endp (padded));
  return padded;
}

/* Scan the given packet for HMAC TLVs having KeyID and Length fields fitting
 * the provided ESA. Return 1 if such TLVs exist and at least one has its Digest
 * field matching a locally-computed HMAC digest of the padded version of the
 * packet. Return 0 otherwise. */
static int
babel_auth_try_hmac_tlvs
(
  struct babel_auth_stats *if_stats,
  struct stream *packet,      /* original packet                  */
  struct stream *padded,      /* padded copy                      */
  struct babel_esa_item *esa, /* current ESA                      */
  unsigned *done              /* digests computed for this packet */
)
{
  u_int8_t tlv_type;
  u_int8_t tlv_length;
  u_int16_t tlv_key_id;
  u_int8_t local_digest[HASH_SIZE_MAX];
  unsigned got_local_digest = 0;
  char printbuf[2 * HASH_SIZE_MAX + 1];
  unsigned i;

  if (*done == BABEL_MAXDIGESTSIN)
    return MSG_NG;
  stream_set_getp (packet, 4);
  while (STREAM_READABLE (packet))
  {
    tlv_type = stream_getc (packet);
    if (tlv_type == MESSAGE_PAD1)
      continue;
    tlv_length = stream_getc (packet);
    if (tlv_type != MESSAGE_HMAC || tlv_length != hash_digest_length[esa->hash_algo] + 2)
    {
      stream_forward_getp (packet, tlv_length);
      continue;
    }
    tlv_key_id = stream_getw (packet);
    if (tlv_key_id != esa->key_id)
    {
      stream_forward_getp (packet, tlv_length - 2);
      continue;
    }
    /* fits scan criterias */
    if (! got_local_digest)
    {
      /* Computation of local digest is lazy and happens only once for a given
       * ESA. Number of computations done for a given packet is limited. */
      unsigned hash_err = hash_make_hmac
      (
        esa->hash_algo,
        stream_get_data (padded), stream_get_endp (padded), /* message */
        esa->key_secret, esa->key_len,                      /* key     */
        local_digest                                        /* result  */
      );
      if (hash_err)
      {
        zlog_err ("%s: hash function error %u", __func__, hash_err);
        stats.internal_err++;
        if_stats->internal_err++;
        return MSG_NG;
      }
      (*done)++;
      got_local_digest = 1;
      if (UNLIKELY (debug & BABEL_DEBUG_AUTH))
      {
        for (i = 0; i + 2 < tlv_length; i++)
          snprintf (printbuf + i * 2, 3, "%02X", local_digest[i]);
        zlog_debug ("%s: local %s digest result #%u%s: %s", __func__,
                    LOOKUP (hash_algo_str, esa->hash_algo), *done,
                    (*done == BABEL_MAXDIGESTSIN ? " (last)" : ""), printbuf);
      }
    }
    debugf (BABEL_DEBUG_AUTH, "%s: HMAC TLV with KeyID %u, digest size %u",
            __func__, tlv_key_id, tlv_length - 2);
    /* OK to compare Digest field */
    if (! memcmp (stream_get_data (packet) + stream_get_getp (packet), local_digest, tlv_length - 2))
    {
      debugf (BABEL_DEBUG_AUTH, "%s: TLV digest matches", __func__);
      return MSG_OK;
    }
    if (UNLIKELY (debug & BABEL_DEBUG_AUTH))
    {
      for (i = 0; i + 2 < tlv_length; i++)
        snprintf (printbuf + i * 2, 3, "%02X", stream_get_data (packet)[stream_get_getp (packet) + i]);
      zlog_debug ("%s: TLV digest differs: %s", __func__, printbuf);
    }
    stream_forward_getp (packet, tlv_length - 2);
  }
  return MSG_NG;
}

/* Check given packet to be authentic, that is, to bear at least one TS/PC TLV,
 * to have the first TS/PC TLV passed ANM check, to bear at least one HMAC TLV,
 * to have at least one HMAC TLV passed HMAC check (done against the original
 * packet after a padding procedure involving the IPv6 address of the sender).
 * Take care of performing a HMAC procedure at most MaxDigestsIn times. */
int babel_auth_check_packet
(
  struct interface *ifp,      /* inbound interface      */
  const unsigned char *from,  /* IPv6 address of sender */
  const unsigned char *input, /* received packet data   */
  const u_int16_t packetlen
)
{
  struct babel_interface *babel_ifp = ifp->info;
  struct stream *packet, *padded;
  struct list *esalist;
  struct babel_esa_item *esa;
  struct babel_anm_item *anm;
  struct listnode *node;
  int tspc_getp, result = MSG_NG;
  u_int16_t neigh_pc = 0;
  u_int32_t neigh_ts = 0;
  unsigned digests_done = 0;
  time_t now;

  /* no CSAs => do nothing */
  if (! listcount (babel_ifp->csalist))
  {
    stats.plain_recv++;
    babel_ifp->auth_stats.plain_recv++;
    return MSG_OK;
  }
  debugf (BABEL_DEBUG_AUTH, "%s: packet length is %uB", __func__, packetlen);
  /* original packet */
  packet = stream_new (packetlen);
  stream_put (packet, input, packetlen);
  /* verify TS/PC before proceeding to expensive checks */
  if (NULL != (anm = babel_anm_lookup ((const struct in6_addr *)from, ifp)))
  {
    neigh_pc = anm->last_pc;
    neigh_ts = anm->last_ts;
  }
  if (-1 == (tspc_getp = babel_auth_check_tspc (&babel_ifp->auth_stats, packet, neigh_pc, neigh_ts)))
  {
    stream_free (packet);
    return babel_ifp->authrxreq ? MSG_NG : MSG_OK;
  }
  /* Pin' := Pin; pad Pin' */
  padded = babel_auth_pad_packet (packet, from);
  /* build ESA list */
  now = quagga_time (NULL);
  esalist = babel_esalist_derive (babel_ifp->csalist, now, keys_valid_for_accept);
  debugf (BABEL_DEBUG_AUTH, "%s: %u ESAs available", __func__, listcount (esalist));
  if (! listcount (esalist))
  {
    stats.auth_recv_ng_nokeys++;
    babel_ifp->auth_stats.auth_recv_ng_nokeys++;
    zlog_warn ("interface %s has no valid keys", ifp->name);
  }
  /* try Pin HMAC TLVs against ESA list and Pin' */
  for (ALL_LIST_ELEMENTS_RO (esalist, node, esa))
    if (MSG_OK == (result = babel_auth_try_hmac_tlvs (&babel_ifp->auth_stats, packet,
                                                      padded, esa, &digests_done)))
      break;
  list_delete (esalist);
  stream_free (padded);
  debugf (BABEL_DEBUG_AUTH, "%s: authentication %s", __func__, result == MSG_OK ? "OK" : "failed");
  if (result != MSG_OK)
  {
    stats.auth_recv_ng_hmac++;
    babel_ifp->auth_stats.auth_recv_ng_hmac++;
  }
  else
  {
    anm = babel_anm_get ((const struct in6_addr *)from, ifp); /* may create new */
    anm->last_pc = stream_getw_from (packet, tspc_getp);
    anm->last_ts = stream_getl_from (packet, tspc_getp + 2);
    anm->last_recv = now;
    stats.auth_recv_ok++;
    babel_ifp->auth_stats.auth_recv_ok++;
    debugf (BABEL_DEBUG_AUTH, "%s: updated neighbor TS/PC to (%u/%u)", __func__,
            anm->last_ts, anm->last_pc);
  }
  stream_free (packet);
  return babel_ifp->authrxreq ? result : MSG_OK;
}

/* Return one of link-local IPv6 addresses belonging to the given interface or
 * fail when there is none. The address will be used to pad first 16 bytes of
 * Digest field of HMAC TLVs.
 *
 * FIXME: In this implementation having more than 1 link-local IPv6 address per
 * Babel interface can cause producing "authenticated" packets, which will never
 * pass an authentication check (because the address picked by this function may
 * be different from the real packet source address). Properly coupling sending
 * and authentication processes for the case of multiple link-local addresses is
 * left for a future work round. */
static int
babel_auth_got_source_address (const struct interface *ifp, unsigned char * addr)
{
  struct listnode *node;
  struct connected *connected;
  char buffer[INET6_ADDRSTRLEN];

  FOR_ALL_INTERFACES_ADDRESSES (ifp, connected, node)
    if
    (
      connected->address->family == AF_INET6 &&
      connected->address->prefixlen == 64 &&
      linklocal (connected->address->u.prefix6.s6_addr)
    )
    {
      debugf (BABEL_DEBUG_AUTH, "%s: using link-local address %s", __func__,
              inet_ntop (AF_INET6, &connected->address->u.prefix6, buffer, INET6_ADDRSTRLEN));
      IPV6_ADDR_COPY ((struct in6_addr *)addr, &connected->address->u.prefix6);
      return 1;
    }
  /* Reaching here means either a logic error or a race condition, because
   * sending Babel packets implies having at least one link-local IPv6 address
   * on the outgoing interface. */
  zlog_err ("%s: no link-local addresses present on interface %s", __func__, ifp->name);
  return 0;
}

/* Bump local routing process TS/PC variables before authenticating next packet. */
static void
babel_auth_bump_tspc (struct babel_interface *babel_ifp, const time_t now)
{
  switch (ts_base)
  {
  case BABEL_TS_BASE_UNIX:
    if (now > babel_ifp->auth_timestamp)
    {
      babel_ifp->auth_timestamp = now;
      babel_ifp->auth_packetcounter = 0;
      return;
    }
    /* otherwise keep counting */
  case BABEL_TS_BASE_ZERO:
    if (++babel_ifp->auth_packetcounter == 0)
      ++babel_ifp->auth_timestamp;
    return;
  }
}
/* Compute and append authentication TLVs to the given packet and return new
 * packet length. New TLVs are one TS/PC TLV per packet and one HMAC TLV for each
 * (but not more than MaxDigestsOut) ESA. HMAC procedure is performed on a copy
 * of the packet after a padding procedure involving the IPv6 address of the
 * sender. */
int babel_auth_make_packet (struct interface *ifp, unsigned char * body, const u_int16_t body_len)
{
  struct babel_interface *babel_ifp = ifp->info;
  struct stream *packet, *padded;
  struct list *esalist;
  struct babel_esa_item *esa;
  struct listnode *node;
  unsigned i, hmacs_done;
  size_t digest_offset[BABEL_MAXDIGESTSOUT];
  int new_body_len;
  char printbuf[2 * HASH_SIZE_MAX + 1];
  struct in6_addr sourceaddr;
  time_t now;

  /* no CSAs or no IPv6 addresses => do nothing */
  if (! listcount (babel_ifp->csalist))
  {
    stats.plain_sent++;
    babel_ifp->auth_stats.plain_sent++;
    return body_len;
  }
  if (! babel_auth_got_source_address (ifp, sourceaddr.s6_addr))
  {
    stats.internal_err++;
    babel_ifp->auth_stats.internal_err++;
    return body_len;
  }
  /* build ESA list */
  now = quagga_time (NULL);
  esalist = babel_esalist_derive (babel_ifp->csalist, now, keys_valid_for_send);
  debugf (BABEL_DEBUG_AUTH, "%s: %u ESAs available", __func__, listcount (esalist));
  if (! listcount (esalist))
  {
    stats.auth_sent_ng_nokeys++;
    babel_ifp->auth_stats.auth_sent_ng_nokeys++;
    zlog_warn ("interface %s has no valid keys", ifp->name);
  }
  debugf (BABEL_DEBUG_AUTH, "%s: original body length is %uB", __func__, body_len);
  /* packet header, original body, authentication TLVs */
  packet = stream_new (4 + body_len + BABEL_MAXAUTHSPACE);
  stream_putc (packet, RTPROT_BABEL);
  stream_putc (packet, 2);
  stream_putw (packet, 0); /* body length placeholder */
  stream_put (packet, body, body_len);
  /* append TS/PC TLV */
  babel_auth_bump_tspc (babel_ifp, now);
  stream_putc (packet, MESSAGE_TSPC);
  stream_putc (packet, 6);
  stream_putw (packet, babel_ifp->auth_packetcounter);
  stream_putl (packet, babel_ifp->auth_timestamp);
  debugf (BABEL_DEBUG_AUTH, "%s: appended TS/PC TLV (%u/%u)", __func__,
          babel_ifp->auth_timestamp, babel_ifp->auth_packetcounter);
  /* HMAC: append up to MaxDigestsOut placeholder TLVs */
  hmacs_done = 0;
  for (ALL_LIST_ELEMENTS_RO (esalist, node, esa))
  {
    debugf (BABEL_DEBUG_AUTH, "%s: padded HMAC TLV #%u (%s, ID %u) at offset %zu",
            __func__, hmacs_done, LOOKUP (hash_algo_str, esa->hash_algo),
            esa->key_id, stream_get_endp (packet));
    stream_putc (packet, MESSAGE_HMAC); /* type */
    stream_putc (packet, 2 + hash_digest_length[esa->hash_algo]); /* length */
    stream_putw (packet, esa->key_id); /* KeyID */
    digest_offset[hmacs_done] = stream_get_endp (packet);
    stream_put (packet, &sourceaddr.s6_addr, IPV6_MAX_BYTELEN);
    stream_put (packet, NULL, hash_digest_length[esa->hash_algo] - IPV6_MAX_BYTELEN);
    if (++hmacs_done == BABEL_MAXDIGESTSOUT)
      break;
  }
  /* time to fill in new body length */
  new_body_len = stream_get_endp (packet) - 4;
  debugf (BABEL_DEBUG_AUTH, "%s: authenticated body length is %uB", __func__, new_body_len);
  stream_putw_at (packet, 2, new_body_len);
  /* Pin' := Pin */
  padded = stream_dup (packet);
  /* fill in pending digests */
  hmacs_done = 0;
  for (ALL_LIST_ELEMENTS_RO (esalist, node, esa))
  {
    unsigned hash_err = hash_make_hmac
    (
      esa->hash_algo,
      stream_get_data (padded), stream_get_endp (padded),  /* message */
      esa->key_secret, esa->key_len,                       /* key     */
      stream_get_data (packet) + digest_offset[hmacs_done] /* result  */
    );
    if (hash_err)
    {
      debugf (BABEL_DEBUG_AUTH, "%s: hash function error %u", __func__, hash_err);
      list_delete (esalist);
      stream_free (padded);
      stream_free (packet);
      stats.internal_err++;
      babel_ifp->auth_stats.internal_err++;
      return body_len;
    }
    if (UNLIKELY (debug & BABEL_DEBUG_AUTH))
    {
      for (i = 0; i < hash_digest_length[esa->hash_algo]; i++)
        snprintf (printbuf + i * 2, 3, "%02X",
                  stream_get_data (packet)[digest_offset[hmacs_done] + i]);
      zlog_debug ("%s: digest #%u at offset %zu: %s", __func__,
                  hmacs_done, digest_offset[hmacs_done], printbuf);
    }
    if (++hmacs_done == BABEL_MAXDIGESTSOUT)
      break;
  }
  list_delete (esalist);
  stream_free (padded);
  /* append new TLVs to the original body */
  memcpy (body + body_len, stream_get_data (packet) + 4 + body_len, new_body_len - body_len);
  stream_free (packet);
  stats.auth_sent++;
  babel_ifp->auth_stats.auth_sent++;
  return new_body_len;
}
#endif /* HAVE_LIBGCRYPT */

void
show_babel_auth_parameters (struct vty *vty)
{
    vty_out(vty,
            "MaxDigestsIn            = %u%s"
            "MaxDigestsOut           = %u%s"
            "Timestamp base          = %s%s"
            "Memory timeout          = %u%s",
            BABEL_MAXDIGESTSIN, VTY_NEWLINE,
            BABEL_MAXDIGESTSOUT, VTY_NEWLINE,
            LOOKUP (ts_base_str, ts_base), VTY_NEWLINE,
            anm_timeout, VTY_NEWLINE);
}

DEFUN (anm_timeout_val,
       anm_timeout_val_cmd,
       "anm-timeout <5-4294967295>",
       "Authentic neighbors memory\n"
       "Timeout in seconds")
{
  VTY_GET_INTEGER_RANGE ("timeout", anm_timeout, argv[0], 5, UINT32_MAX);
  return CMD_SUCCESS;
}

DEFUN (no_anm_timeout_val,
       no_anm_timeout_val_cmd,
       "no anm-timeout <5-4294967295>",
       NO_STR
       "Authentic neighbors memory\n"
       "Timeout in seconds")
{
  anm_timeout = BABEL_DEFAULT_ANM_TIMEOUT;
  return CMD_SUCCESS;
}

ALIAS (no_anm_timeout_val,
       no_anm_timeout_cmd,
       "no anm-timeout",
       NO_STR
       "Authentic neighbors memory\n"
       "Timeout in seconds")

DEFUN (ts_base_val,
       ts_base_val_cmd,
       "ts-base (zero|unixtime)",
       "Packet timestamp base\n"
       "NVRAM-less PC wrap counter\n"
       "UNIX time w/PC wrap counter")
{
  if (! strcmp (argv[0], "zero"))
    ts_base = BABEL_TS_BASE_ZERO;
  else if (! strcmp (argv[0], "unixtime"))
    ts_base = BABEL_TS_BASE_UNIX;
  return CMD_SUCCESS;
}

DEFUN (no_ts_base,
       no_ts_base_val_cmd,
       "no ts-base (zero|unixtime)",
       NO_STR
       "Packet timestamp base\n"
       "NVRAM-less PC wrap counter\n"
       "UNIX time w/PC wrap counter")
{
  ts_base = BABEL_DEFAULT_TS_BASE;
  return CMD_SUCCESS;
}

ALIAS (no_ts_base,
       no_ts_base_cmd,
       "no ts-base",
       NO_STR
       "Packet timestamp base")

static void
show_auth_stats_sub (struct vty *vty, const struct babel_auth_stats stats)
{
  const char *format_lu = "%-32s: %lu%s";

  vty_out (vty, format_lu, "Plain Rx", stats.plain_recv, VTY_NEWLINE);
  vty_out (vty, format_lu, "Plain Tx", stats.plain_sent, VTY_NEWLINE);
  vty_out (vty, format_lu, "Authenticated Tx OK", stats.auth_sent, VTY_NEWLINE);
  vty_out (vty, format_lu, "Authenticated Tx out of keys", stats.auth_sent_ng_nokeys, VTY_NEWLINE);
  vty_out (vty, format_lu, "Authenticated Rx OK", stats.auth_recv_ok, VTY_NEWLINE);
  vty_out (vty, format_lu, "Authenticated Rx out of keys", stats.auth_recv_ng_nokeys, VTY_NEWLINE);
  vty_out (vty, format_lu, "Authenticated Rx missing TS/PC", stats.auth_recv_ng_no_tspc, VTY_NEWLINE);
  vty_out (vty, format_lu, "Authenticated Rx bad TS/PC", stats.auth_recv_ng_tspc, VTY_NEWLINE);
  vty_out (vty, format_lu, "Authenticated Rx bad HMAC", stats.auth_recv_ng_hmac, VTY_NEWLINE);
  vty_out (vty, format_lu, "Internal errors", stats.internal_err, VTY_NEWLINE);
}

DEFUN (show_babel_authentication_stats,
       show_babel_authentication_stats_cmd,
       "show babel authentication stats",
       SHOW_STR
       "Babel information\n"
       "Packet authentication\n"
       "Authentication statistics\n")
{
  vty_out (vty, "== Authentication statistics for this Babel speaker ==%s", VTY_NEWLINE);
  show_auth_stats_sub (vty, stats);
  return CMD_SUCCESS;
}

DEFUN (show_babel_authentication_stats_interface,
       show_babel_authentication_stats_interface_cmd,
       "show babel authentication stats interface",
       SHOW_STR
       "Babel information\n"
       "Packet authentication\n"
       "Authentication statistics\n"
       "Per-interface statistics\n")
{
  struct listnode *node;
  struct interface *ifp;

  for (ALL_LIST_ELEMENTS_RO (iflist, node, ifp))
    if (babel_enable_if_lookup (ifp->name) >= 0)
    {
      struct babel_interface *babel_ifp = ifp->info;
      vty_out (vty, "== Authentication statistics for interface %s ==%s", ifp->name, VTY_NEWLINE);
      show_auth_stats_sub (vty, babel_ifp->auth_stats);
    }
  return CMD_SUCCESS;
}

DEFUN (show_babel_authentication_stats_interface_val,
       show_babel_authentication_stats_interface_val_cmd,
       "show babel authentication stats interface IFNAME",
       SHOW_STR
       "Babel information\n"
       "Packet authentication\n"
       "Authentication statistics\n"
       "Per-interface statistics\n"
       "Interface name\n")
{
  struct listnode *node;
  struct interface *ifp;

  for (ALL_LIST_ELEMENTS_RO (iflist, node, ifp))
    if (! strcmp (argv[0], ifp->name))
    {
      struct babel_interface *babel_ifp = ifp->info;

      if (babel_enable_if_lookup (ifp->name) < 0)
      {
        vty_out (vty, "Interface %s is not a Babel interface%s", argv[0], VTY_NEWLINE);
        return CMD_WARNING;
      }
      vty_out (vty, "== Authentication statistics for interface %s ==%s", argv[0], VTY_NEWLINE);
      show_auth_stats_sub (vty, babel_ifp->auth_stats);
      return CMD_SUCCESS;
    }
  vty_out (vty, "Interface %s not found%s", argv[0], VTY_NEWLINE);
  return CMD_WARNING;
}

DEFUN (clear_babel_authentication_stats,
       clear_babel_authentication_stats_cmd,
       "clear babel authentication stats",
       CLEAR_STR
       "Babel information\n"
       "Packet authentication\n"
       "Authentication statistics")
{
  memset (&stats, 0, sizeof (stats));
  return CMD_SUCCESS;
}

DEFUN (clear_babel_authentication_stats_interface_val,
       clear_babel_authentication_stats_interface_val_cmd,
       "clear babel authentication stats interface IFNAME",
       CLEAR_STR
       "Babel information\n"
       "Packet authentication\n"
       "Authentication statistics\n"
       "Per-interface statistics\n"
       "Interface name\n")
{
  struct listnode *node;
  struct interface *ifp;

  for (ALL_LIST_ELEMENTS_RO (iflist, node, ifp))
    if (! strcmp (argv[0], ifp->name))
    {
      struct babel_interface *babel_ifp = ifp->info;

      if (babel_enable_if_lookup (ifp->name) < 0)
      {
        vty_out (vty, "Interface %s is not a Babel interface%s", argv[0], VTY_NEWLINE);
        return CMD_WARNING;
      }
      memset (&babel_ifp->auth_stats, 0, sizeof (struct babel_auth_stats));
      return CMD_SUCCESS;
    }
  vty_out (vty, "Interface %s not found%s", argv[0], VTY_NEWLINE);
  return CMD_WARNING;
}

DEFUN (clear_babel_authentication_stats_interface,
       clear_babel_authentication_stats_interface_cmd,
       "clear babel authentication stats interface",
       CLEAR_STR
       "Babel information\n"
       "Packet authentication\n"
       "Authentication statistics\n"
       "Per-interface statistics\n")
{
  struct listnode *node;
  struct interface *ifp;

  for (ALL_LIST_ELEMENTS_RO (iflist, node, ifp))
    if (babel_enable_if_lookup (ifp->name) >= 0)
    {
      struct babel_interface *babel_ifp = ifp->info;
      memset (&babel_ifp->auth_stats, 0, sizeof (struct babel_auth_stats));
    }
  return CMD_SUCCESS;
}

DEFUN (show_babel_authentication_memory,
       show_babel_authentication_memory_cmd,
       "show babel authentication memory",
       SHOW_STR
       "Babel information\n"
       "Packet authentication\n"
       "Authentic neighbors memory")
{
  struct listnode *node;
  struct babel_anm_item *anm;
  char buffer[INET6_ADDRSTRLEN];
  time_t now = quagga_time (NULL);
  const char *format_s = "%46s %10s %10s %5s %10s%s";
  const char *format_u = "%46s %10s %10u %5u %10u%s";

  vty_out (vty, "ANM timeout: %u seconds, ANM records: %u%s", anm_timeout,
           listcount (anmlist), VTY_NEWLINE);
  vty_out (vty, format_s, "Source address", "Interface", "TS", "PC", "Age", VTY_NEWLINE);
  for (ALL_LIST_ELEMENTS_RO (anmlist, node, anm))
  {
    inet_ntop (AF_INET6, &anm->address, buffer, INET6_ADDRSTRLEN);
    vty_out (vty, format_u, buffer, anm->ifp->name, anm->last_ts, anm->last_pc,
             now - anm->last_recv, VTY_NEWLINE);
  }
  return CMD_SUCCESS;
}

DEFUN (clear_babel_authentication_memory,
       clear_babel_authentication_memory_cmd,
       "clear babel authentication memory",
       CLEAR_STR
       "Babel information\n"
       "Packet authentication\n"
       "Authentic neighbors memory")
{
  list_delete_all_node (anmlist);
  return CMD_SUCCESS;
}

int
babel_auth_config_write (struct vty *vty)
{
  int lines = 0;

  if (anm_timeout != BABEL_DEFAULT_ANM_TIMEOUT)
  {
      vty_out (vty, " anm-timeout %u%s", anm_timeout, VTY_NEWLINE);
      lines++;
  }
  if (ts_base != BABEL_DEFAULT_TS_BASE)
  {
      vty_out (vty, " ts-base %s%s", LOOKUP (ts_base_cli_str, ts_base), VTY_NEWLINE);
      lines++;
  }
  return lines;
}

void
babel_auth_init()
{
  if (hash_library_init())
    exit (1);
  anmlist = list_new();
  anmlist->del = babel_anm_free;
  memset (&stats, 0, sizeof (stats));
  anm_timeout = BABEL_DEFAULT_ANM_TIMEOUT;
  ts_base = BABEL_DEFAULT_TS_BASE;
  install_element (BABEL_NODE, &anm_timeout_val_cmd);
  install_element (BABEL_NODE, &no_anm_timeout_val_cmd);
  install_element (BABEL_NODE, &no_anm_timeout_cmd);
  install_element (BABEL_NODE, &ts_base_val_cmd);
  install_element (BABEL_NODE, &no_ts_base_val_cmd);
  install_element (BABEL_NODE, &no_ts_base_cmd);
  install_element (VIEW_NODE, &show_babel_authentication_stats_cmd);
  install_element (VIEW_NODE, &show_babel_authentication_stats_interface_cmd);
  install_element (VIEW_NODE, &show_babel_authentication_stats_interface_val_cmd);
  install_element (VIEW_NODE, &show_babel_authentication_memory_cmd);
  install_element (ENABLE_NODE, &show_babel_authentication_stats_cmd);
  install_element (ENABLE_NODE, &show_babel_authentication_stats_interface_cmd);
  install_element (ENABLE_NODE, &show_babel_authentication_stats_interface_val_cmd);
  install_element (ENABLE_NODE, &show_babel_authentication_memory_cmd);
  install_element (ENABLE_NODE, &clear_babel_authentication_stats_cmd);
  install_element (ENABLE_NODE, &clear_babel_authentication_stats_interface_cmd);
  install_element (ENABLE_NODE, &clear_babel_authentication_stats_interface_val_cmd);
  install_element (ENABLE_NODE, &clear_babel_authentication_memory_cmd);
}
