/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2006-2009 Collabora Ltd.
 *  Contact: Youness Alaoui
 * (C) 2006-2009 Nokia Corporation. All rights reserved.
 *  Contact: Kai Vehmanen
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Nice GLib ICE library.
 *
 * The Initial Developers of the Original Code are Collabora Ltd and Nokia
 * Corporation. All Rights Reserved.
 *
 * Contributors:
 *   Kai Vehmanen, Nokia
 *   Youness Alaoui, Collabora Ltd.
 *   Dafydd Harries, Collabora Ltd.
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * the GNU Lesser General Public License Version 2.1 (the "LGPL"), in which
 * case the provisions of LGPL are applicable instead of those above. If you
 * wish to allow use of your version of this file only under the terms of the
 * LGPL and not to allow others to use your version of this file under the
 * MPL, indicate your decision by deleting the provisions above and replace
 * them with the notice and other provisions required by the LGPL. If you do
 * not delete the provisions above, a recipient may use your version of this
 * file under either the MPL or the LGPL.
 */

/*
 * @file conncheck.c
 * @brief ICE connectivity checks
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <string.h>

#include <glib.h>

#include "debug.h"

#include "agent.h"
#include "agent-priv.h"
#include "conncheck.h"
#include "discovery.h"
#include "stun/stun5389.h"
#include "stun/usages/ice.h"
#include "stun/usages/bind.h"
#include "stun/usages/turn.h"

static void priv_update_check_list_failed_components (NiceAgent *agent, NiceStream *stream);
static guint priv_prune_pending_checks (NiceAgent *agent, NiceStream *stream, NiceComponent *component);
static gboolean priv_schedule_triggered_check (NiceAgent *agent, NiceStream *stream, NiceComponent *component, NiceSocket *local_socket, NiceCandidate *remote_cand);
static void priv_mark_pair_nominated (NiceAgent *agent, NiceStream *stream, NiceComponent *component, NiceCandidate *localcand, NiceCandidate *remotecand);
static size_t priv_create_username (NiceAgent *agent, NiceStream *stream,
    guint component_id, NiceCandidate *remote, NiceCandidate *local,
    uint8_t *dest, guint dest_len, gboolean inbound);
static size_t priv_get_password (NiceAgent *agent, NiceStream *stream,
    NiceCandidate *remote, uint8_t **password);
static void candidate_check_pair_free (NiceAgent *agent,
    CandidateCheckPair *pair);
static CandidateCheckPair *priv_conn_check_add_for_candidate_pair_matched (
    NiceAgent *agent, guint stream_id, NiceComponent *component,
    NiceCandidate *local, NiceCandidate *remote, NiceCheckState initial_state);
static gboolean priv_conn_keepalive_tick_agent_locked (NiceAgent *agent,
    gpointer pointer);

static gint64 priv_timer_remainder (gint64 timer, gint64 now)
{
  if (now >= timer)
    return 0;

  return (timer - now) / 1000;
}

static gchar
priv_state_to_gchar (NiceCheckState state)
{
  switch (state) {
    case NICE_CHECK_WAITING:
      return 'W';
    case NICE_CHECK_IN_PROGRESS:
      return 'I';
    case NICE_CHECK_SUCCEEDED:
      return 'S';
    case NICE_CHECK_FAILED:
      return 'F';
    case NICE_CHECK_FROZEN:
      return 'Z';
    case NICE_CHECK_DISCOVERED:
      return 'D';
    default:
      g_assert_not_reached ();
  }
}

static const gchar *
priv_state_to_string (NiceCheckState state)
{
  switch (state) {
    case NICE_CHECK_WAITING:
      return "WAITING";
    case NICE_CHECK_IN_PROGRESS:
      return "IN_PROGRESS";
    case NICE_CHECK_SUCCEEDED:
      return "SUCCEEDED";
    case NICE_CHECK_FAILED:
      return "FAILED";
    case NICE_CHECK_FROZEN:
      return "FROZEN";
    case NICE_CHECK_DISCOVERED:
      return "DISCOVERED";
    default:
      g_assert_not_reached ();
  }
}

#define SET_PAIR_STATE( a, p, s ) G_STMT_START{\
  g_assert (p); \
  p->state = s; \
  nice_debug ("Agent %p : pair %p state %s (%s)", \
      a, p, priv_state_to_string (s), G_STRFUNC); \
}G_STMT_END

static const gchar *
priv_ice_return_to_string (StunUsageIceReturn ice_return)
{
  switch (ice_return) {
    case STUN_USAGE_ICE_RETURN_SUCCESS:
      return "success";
    case STUN_USAGE_ICE_RETURN_ERROR:
      return "error";
    case STUN_USAGE_ICE_RETURN_INVALID:
      return "invalid";
    case STUN_USAGE_ICE_RETURN_ROLE_CONFLICT:
      return "role conflict";
    case STUN_USAGE_ICE_RETURN_INVALID_REQUEST:
      return "invalid request";
    case STUN_USAGE_ICE_RETURN_INVALID_METHOD:
      return "invalid method";
    case STUN_USAGE_ICE_RETURN_MEMORY_ERROR:
      return "memory error";
    case STUN_USAGE_ICE_RETURN_INVALID_ADDRESS:
      return "invalid address";
    case STUN_USAGE_ICE_RETURN_NO_MAPPED_ADDRESS:
      return "no mapped address";
    default:
      g_assert_not_reached ();
  }
}

static const gchar *
priv_candidate_type_to_string (NiceCandidateType type)
{
  switch (type) {
    case NICE_CANDIDATE_TYPE_HOST:
      return "host";
    case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
      return "srflx";
    case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE:
      return "prflx";
    case NICE_CANDIDATE_TYPE_RELAYED:
      return "relay";
    default:
      g_assert_not_reached ();
  }
}

static const gchar *
priv_candidate_transport_to_string (NiceCandidateTransport transport)
{
  switch (transport) {
    case NICE_CANDIDATE_TRANSPORT_UDP:
      return "udp";
    case NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE:
      return "tcp-act";
    case NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE:
      return "tcp-pass";
    case NICE_CANDIDATE_TRANSPORT_TCP_SO:
      return "tcp-so";
    default:
      g_assert_not_reached ();
  }
}

static const gchar *
priv_socket_type_to_string (NiceSocketType type)
{
  switch (type) {
    case NICE_SOCKET_TYPE_UDP_BSD:
      return "udp";
    case NICE_SOCKET_TYPE_TCP_BSD:
      return "tcp";
    case NICE_SOCKET_TYPE_PSEUDOSSL:
      return "ssl";
    case NICE_SOCKET_TYPE_HTTP:
      return "http";
    case NICE_SOCKET_TYPE_SOCKS5:
      return "socks";
    case NICE_SOCKET_TYPE_UDP_TURN:
      return "udp-turn";
    case NICE_SOCKET_TYPE_UDP_TURN_OVER_TCP:
      return "tcp-turn";
    case NICE_SOCKET_TYPE_TCP_ACTIVE:
      return "tcp-act";
    case NICE_SOCKET_TYPE_TCP_PASSIVE:
      return "tcp-pass";
    case NICE_SOCKET_TYPE_TCP_SO:
      return "tcp-so";
    default:
      g_assert_not_reached ();
  }
}

/*
 * Dump the component list of incoming checks
 */
static void
print_component_incoming_checks (NiceAgent *agent, NiceStream *stream,
  NiceComponent *component)
{
  GList *i;

  for (i = component->incoming_checks.head; i; i = i->next) {
    IncomingCheck *icheck = i->data;
    gchar tmpbuf1[INET6_ADDRSTRLEN] = {0};
    gchar tmpbuf2[INET6_ADDRSTRLEN] = {0};

    nice_address_to_string (&icheck->local_socket->addr, tmpbuf1);
    nice_address_to_string (&icheck->from, tmpbuf2);
    nice_debug ("Agent %p : *** sc=%d/%d : icheck %p : "
      "sock %s [%s]:%u > [%s]:%u",
      agent, stream->id, component->id, icheck,
      priv_socket_type_to_string (icheck->local_socket->type),
      tmpbuf1, nice_address_get_port (&icheck->local_socket->addr),
      tmpbuf2, nice_address_get_port (&icheck->from));
  }
}

/*
 * Dump the conncheck lists of the agent
 */
static void
priv_print_conn_check_lists (NiceAgent *agent, const gchar *where, const gchar *detail)
{
  GSList *i, *k, *l;
  guint j, m;
  gint64 now;

  if (!nice_debug_is_verbose ())
    return;

  now = g_get_monotonic_time ();

#define PRIORITY_LEN 32

  nice_debug ("Agent %p : *** conncheck list DUMP (called from %s%s)",
      agent, where, detail ? detail : "");
  nice_debug ("Agent %p : *** agent nomination mode %s, %s",
      agent, agent->nomination_mode == NICE_NOMINATION_MODE_AGGRESSIVE ?
      "aggressive" : "regular",
      agent->controlling_mode ? "controlling" : "controlled");
  for (i = agent->streams; i ; i = i->next) {
    NiceStream *stream = i->data;
    for (j = 1; j <= stream->n_components; j++) {
      NiceComponent *component;
      for (k = stream->conncheck_list; k ; k = k->next) {
        CandidateCheckPair *pair = k->data;
        if (pair->component_id == j) {
          gchar local_addr[INET6_ADDRSTRLEN];
          gchar remote_addr[INET6_ADDRSTRLEN];
          gchar priority[NICE_CANDIDATE_PAIR_PRIORITY_MAX_SIZE];

          nice_address_to_string (&pair->local->addr, local_addr);
          nice_address_to_string (&pair->remote->addr, remote_addr);
          nice_candidate_pair_priority_to_string (pair->priority, priority);

          nice_debug ("Agent %p : *** sc=%d/%d : pair %p : "
              "f=%s t=%s:%s sock=%s "
              "%s:[%s]:%u > %s:[%s]:%u prio=%s/%08x state=%c%s%s%s%s",
              agent, pair->stream_id, pair->component_id, pair,
              pair->foundation,
              priv_candidate_type_to_string (pair->local->type),
              priv_candidate_type_to_string (pair->remote->type),
              priv_socket_type_to_string (pair->sockptr->type),
              priv_candidate_transport_to_string (pair->local->transport),
              local_addr, nice_address_get_port (&pair->local->addr),
              priv_candidate_transport_to_string (pair->remote->transport),
              remote_addr, nice_address_get_port (&pair->remote->addr),
              priority, pair->stun_priority,
              priv_state_to_gchar (pair->state),
              pair->valid ? "V" : "",
              pair->nominated ? "N" : "",
              pair->use_candidate_on_next_check ? "C" : "",
              g_slist_find (agent->triggered_check_queue, pair) ? "T" : "");

          for (l = pair->stun_transactions, m = 0; l; l = l->next, m++) {
            StunTransaction *stun = l->data;
            nice_debug ("Agent %p : *** sc=%d/%d : pair %p :   "
                "stun#=%d timer=%d/%d %" G_GINT64_FORMAT "/%dms buf=%p %s",
                agent, pair->stream_id, pair->component_id, pair, m,
                stun->timer.retransmissions, stun->timer.max_retransmissions,
                stun->timer.delay - priv_timer_remainder (stun->next_tick, now),
                stun->timer.delay,
                stun->message.buffer,
                (m == 0 && pair->retransmit) ? "(R)" : "");
          }
        }
      }
      if (agent_find_component (agent, stream->id, j, NULL, &component))
        print_component_incoming_checks (agent, stream, component);
    }
  }
}

/* Add the pair to the triggered checks list, if not already present
 */
static void
priv_add_pair_to_triggered_check_queue (NiceAgent *agent, CandidateCheckPair *pair)
{
  g_assert (pair);

  if (agent->triggered_check_queue == NULL ||
      g_slist_find (agent->triggered_check_queue, pair) == NULL)
    agent->triggered_check_queue = g_slist_append (agent->triggered_check_queue, pair);
}

/* Remove the pair from the triggered checks list
 */
static void
priv_remove_pair_from_triggered_check_queue (NiceAgent *agent, CandidateCheckPair *pair)
{
  g_assert (pair);
  agent->triggered_check_queue = g_slist_remove (agent->triggered_check_queue, pair);
}

/* Get the pair from the triggered checks list
 */
static CandidateCheckPair *
priv_get_pair_from_triggered_check_queue (NiceAgent *agent)
{
  CandidateCheckPair *pair = NULL;

  if (agent->triggered_check_queue) {
    pair = (CandidateCheckPair *)agent->triggered_check_queue->data;
    priv_remove_pair_from_triggered_check_queue (agent, pair);
  }
  return pair;
}

/*
 * Finds the next connectivity check in WAITING state.
 */
static CandidateCheckPair *priv_conn_check_find_next_waiting (GSList *conn_check_list)
{
  GSList *i;

  /* note: list is sorted in priority order to first waiting check has
   *       the highest priority */
  for (i = conn_check_list; i ; i = i->next) {
    CandidateCheckPair *p = i->data;
    if (p->state == NICE_CHECK_WAITING)
      return p;
  }

  return NULL;
}

/*
 * Initiates a new connectivity check for a ICE candidate pair.
 *
 * @return TRUE on success, FALSE on error
 */
static gboolean
priv_conn_check_initiate (NiceAgent *agent, CandidateCheckPair *pair)
{
  SET_PAIR_STATE (agent, pair, NICE_CHECK_IN_PROGRESS);
  if (conn_check_send (agent, pair)) {
    SET_PAIR_STATE (agent, pair, NICE_CHECK_FAILED);
    return FALSE;
  }
  return TRUE;
}

/*
 * Unfreezes the next connectivity check in the list. Follows the
 * algorithm defined in sect 6.1.2.6 (Computing Candidate Pair States)
 * and sect 6.1.4.2 (Performing Connectivity Checks) of the ICE spec
 * (RFC8445)
 *
 * Note that this algorithm is slightly simplified compared to previous
 * version of the spec (RFC5245), and this new version is now
 * idempotent.
 * 
 * @return TRUE on success, and FALSE if no frozen candidates were found.
 */
static gboolean
priv_conn_check_unfreeze_next (NiceAgent *agent)
{
  GSList *i, *j;
  GSList *foundation_list = NULL;
  gboolean result = FALSE;

  /* While a pair in state waiting exists, we do nothing */
  for (i = agent->streams; i ; i = i->next) {
    NiceStream *s = i->data;
    for (j = s->conncheck_list; j ; j = j->next) {
      CandidateCheckPair *p = j->data;

      if (p->state == NICE_CHECK_WAITING)
        return TRUE;
    }
  }

  /* When there are no more pairs in waiting state, we unfreeze some
   * pairs, so that we get a single waiting pair per foundation.
   */
  for (i = agent->streams; i ; i = i->next) {
    NiceStream *s = i->data;
    for (j = s->conncheck_list; j ; j = j->next) {
      CandidateCheckPair *p = j->data;

      if (g_slist_find_custom (foundation_list, p->foundation,
          (GCompareFunc)strcmp))
        continue;

      if (p->state == NICE_CHECK_FROZEN) {
        nice_debug ("Agent %p : Pair %p with s/c-id %u/%u (%s) unfrozen.",
            agent, p, p->stream_id, p->component_id, p->foundation);
        SET_PAIR_STATE (agent, p, NICE_CHECK_WAITING);
        foundation_list = g_slist_prepend (foundation_list, p->foundation);
        result = TRUE;
      }
    }
  }
  g_slist_free (foundation_list);

  /* We dump the conncheck list when something interesting happened, ie
   * when we unfroze some pairs.
   */
  if (result)
    priv_print_conn_check_lists (agent, G_STRFUNC, NULL);

  return result;
}

/*
 * Unfreezes the related connectivity check in the list after
 * check 'success_check' has successfully completed.
 *
 * See sect 7.2.5.3.3 (Updating Candidate Pair States) of ICE spec (RFC8445).
 *
 * Note that this algorithm is slightly simplified compared to previous
 * version of the spec (RFC5245)
 * 
 * @param agent context
 * @param pair a pair, whose connectivity check has just succeeded
 *
 */
void
conn_check_unfreeze_related (NiceAgent *agent, CandidateCheckPair *pair)
{
  GSList *i, *j;
  gboolean result = FALSE;

  g_assert (pair);
  g_assert (pair->state == NICE_CHECK_SUCCEEDED);

  for (i = agent->streams; i ; i = i->next) {
    NiceStream *s = i->data;
    for (j = s->conncheck_list; j ; j = j->next) {
      CandidateCheckPair *p = j->data;
   
      /* The states for all other Frozen candidates pairs in all
       * checklists with the same foundation is set to waiting
       */
      if (p->state == NICE_CHECK_FROZEN &&
          strncmp (p->foundation, pair->foundation,
              NICE_CANDIDATE_PAIR_MAX_FOUNDATION) == 0) {
        nice_debug ("Agent %p : Unfreezing check %p "
            "(after successful check %p).", agent, p, pair);
        SET_PAIR_STATE (agent, p, NICE_CHECK_WAITING);
        result = TRUE;
      }
    }
  }
  /* We dump the conncheck list when something interesting happened, ie
   * when we unfroze some pairs.
   */
  if (result)
    priv_print_conn_check_lists (agent, G_STRFUNC, NULL);
}

/*
 * Unfreezes this connectivity check if its foundation is the same than
 * the foundation of an already succeeded pair.
 *
 * See sect 7.2.5.3.3 (Updating Candidate Pair States) of ICE spec (RFC8445).
 *
 * @param agent context
 * @param pair a pair, whose state is frozen
 *
 */
static void
priv_conn_check_unfreeze_maybe (NiceAgent *agent, CandidateCheckPair *pair)
{
  GSList *i, *j;
  gboolean result = FALSE;

  g_assert (pair);
  g_assert (pair->state == NICE_CHECK_FROZEN);

  for (i = agent->streams; i ; i = i->next) {
    NiceStream *s = i->data;
    for (j = s->conncheck_list; j ; j = j->next) {
      CandidateCheckPair *p = j->data;

      if (p->state == NICE_CHECK_SUCCEEDED &&
          strncmp (p->foundation, pair->foundation,
              NICE_CANDIDATE_PAIR_MAX_FOUNDATION) == 0) {
        nice_debug ("Agent %p : Unfreezing check %p "
            "(after successful check %p).", agent, pair, p);
        SET_PAIR_STATE (agent, pair, NICE_CHECK_WAITING);
        result = TRUE;
      }
    }
  }
  /* We dump the conncheck list when something interesting happened, ie
   * when we unfroze some pairs.
   */
  if (result)
    priv_print_conn_check_lists (agent, G_STRFUNC, NULL);
}

guint
conn_check_stun_transactions_count (NiceAgent *agent)
{
  GSList *i, *j;
  guint count = 0;

  for (i = agent->streams; i ; i = i->next) {
    NiceStream *s = i->data;
    for (j = s->conncheck_list; j ; j = j->next) {
      CandidateCheckPair *p = j->data;

      if (p->stun_transactions)
        count += g_slist_length (p->stun_transactions);
    }
  }
  return count;
}

/*
 * Create a new STUN transaction and add it to the list
 * of ongoing stun transactions of a pair.
 *
 * @pair the pair the new stun transaction should be added to.
 * @return the created stun transaction.
 */
static StunTransaction *
priv_add_stun_transaction (CandidateCheckPair *pair)
{
  StunTransaction *stun = g_slice_new0 (StunTransaction);
  pair->stun_transactions = g_slist_prepend (pair->stun_transactions, stun);
  pair->retransmit = TRUE;
  return stun;
}

/*
 * Forget a STUN transaction.
 *
 * @data the stun transaction to be forgotten.
 * @user_data the component contained the concerned stun agent.
 */
static void
priv_forget_stun_transaction (gpointer data, gpointer user_data)
{
  StunTransaction *stun = data;
  NiceComponent *component = user_data;
  StunTransactionId id;

  if (stun->message.buffer != NULL) {
    stun_message_id (&stun->message, id);
    stun_agent_forget_transaction (&component->stun_agent, id);
  }
}

static void
priv_free_stun_transaction (gpointer data)
{
  g_slice_free (StunTransaction, data);
}

/*
 * Remove a STUN transaction from a pair, and forget it
 * from the related component stun agent.
 *
 * @pair the pair the stun transaction should be removed from.
 * @stun the stun transaction to be removed.
 * @component the component containing the stun agent used to
 * forget the stun transaction.
 */
static void
priv_remove_stun_transaction (CandidateCheckPair *pair,
  StunTransaction *stun, NiceComponent *component)
{
  priv_forget_stun_transaction (stun, component);
  pair->stun_transactions = g_slist_remove (pair->stun_transactions, stun);
  priv_free_stun_transaction (stun);
  if (pair->stun_transactions == NULL)
    pair->retransmit = FALSE;
}

/*
 * Remove all STUN transactions from a pair, and forget them
 * from the related component stun agent.
 *
 * @pair the pair the stun list should be cleared.
 * @component the component containing the stun agent used to
 * forget the stun transactions.
 */
static void
priv_free_all_stun_transactions (CandidateCheckPair *pair,
  NiceComponent *component)
{
  if (component)
    g_slist_foreach (pair->stun_transactions, priv_forget_stun_transaction, component);
  g_slist_free_full (pair->stun_transactions, priv_free_stun_transaction);
  pair->stun_transactions = NULL;
  pair->retransmit = FALSE;
}

static void
candidate_check_pair_fail (NiceStream *stream, NiceAgent *agent, CandidateCheckPair *p)
{
  NiceComponent *component;

  component = nice_stream_find_component_by_id (stream, p->component_id);
  SET_PAIR_STATE (agent, p, NICE_CHECK_FAILED);
  priv_free_all_stun_transactions (p, component);
}

/*
 * Helper function for connectivity check timer callback that
 * runs through the stream specific part of the state machine. 
 *
 * @param agent context pointer
 * @param stream which stream (of the agent)
 * @return will return TRUE if a new stun request has been sent
 */
static gboolean
priv_conn_check_tick_stream (NiceAgent *agent, NiceStream *stream)
{
  gboolean pair_failed = FALSE;
  GSList *i, *j;
  unsigned int timeout;
  gint64 now;

  now = g_get_monotonic_time ();

  /* step: process ongoing STUN transactions */
  for (i = stream->conncheck_list; i ; i = i->next) {
    CandidateCheckPair *p = i->data;
    gchar tmpbuf1[INET6_ADDRSTRLEN], tmpbuf2[INET6_ADDRSTRLEN];
    NiceComponent *component;
    guint index = 0, remaining = 0;

    if (p->stun_transactions == NULL)
      continue;

    if (!agent_find_component (agent, p->stream_id, p->component_id,
        NULL, &component))
      continue;

    j = p->stun_transactions;
    while (j) {
      StunTransaction *stun = j->data;
      GSList *next = j->next;

      if (now < stun->next_tick)
        remaining++;
      else
        switch (stun_timer_refresh (&stun->timer)) {
          case STUN_USAGE_TIMER_RETURN_TIMEOUT:
timer_return_timeout:
            priv_remove_stun_transaction (p, stun, component);
            break;
          case STUN_USAGE_TIMER_RETURN_RETRANSMIT:
            /* case: retransmission stopped, due to the nomination of
             * a pair with a higher priority than this in-progress pair,
             * ICE spec, sect 8.1.2 "Updating States", item 2.2
             */
            if (!p->retransmit || index > 0)
              goto timer_return_timeout;

            /* case: not ready, so schedule a new timeout */
            timeout = stun_timer_remainder (&stun->timer);

            nice_debug ("Agent %p :STUN transaction retransmitted on pair %p "
                "(timer=%d/%d %d/%dms).",
                agent, p,
                stun->timer.retransmissions, stun->timer.max_retransmissions,
                stun->timer.delay - timeout, stun->timer.delay);

            agent_socket_send (p->sockptr, &p->remote->addr,
                stun_message_length (&stun->message),
                (gchar *)stun->buffer);

            /* note: convert from milli to microseconds for g_time_val_add() */
            stun->next_tick = now + timeout * 1000;

            return TRUE;
          case STUN_USAGE_TIMER_RETURN_SUCCESS:
            timeout = stun_timer_remainder (&stun->timer);
            /* note: convert from milli to microseconds for g_time_val_add() */
            stun->next_tick = now + timeout * 1000;
            remaining++;
            break;
          default:
            g_assert_not_reached();
            break;
        }
      j = next;
      index++;
    }

    if (remaining == 0) {
      nice_address_to_string (&p->local->addr, tmpbuf1);
      nice_address_to_string (&p->remote->addr, tmpbuf2);
      nice_debug ("Agent %p : Retransmissions failed, giving up on pair %p",
          agent, p);
      nice_debug ("Agent %p : Failed pair is [%s]:%u --> [%s]:%u", agent,
          tmpbuf1, nice_address_get_port (&p->local->addr),
          tmpbuf2, nice_address_get_port (&p->remote->addr));
      candidate_check_pair_fail (stream, agent, p);
      pair_failed = TRUE;

      /* perform a check if a transition state from connected to
       * ready can be performed. This may happen here, when the last
       * in-progress pair has expired its retransmission count
       * in priv_conn_check_tick_stream(), which is a condition to
       * make the transition connected to ready.
       */
      conn_check_update_check_list_state_for_ready (agent, stream, component);
    }
  }

  if (pair_failed)
    priv_print_conn_check_lists (agent, G_STRFUNC, ", retransmission failed");

  return FALSE;
}

static gboolean
priv_conn_check_ordinary_check (NiceAgent *agent, NiceStream *stream)
{
  CandidateCheckPair *pair;
  gboolean stun_sent = FALSE;

  /* step: perform an ordinary check, sec 6.1.4.2 point 3. (Performing
   * Connectivity Checks) of ICE spec (RFC8445)
   * note: This code is executed when the triggered checks list is
   * empty, and when no STUN message has been sent (pacing constraint)
   */
  pair = priv_conn_check_find_next_waiting (stream->conncheck_list);
  if (pair == NULL) {
    /* step: there is no candidate in waiting state, try to unfreeze
     * some pairs and retry, sect 6.1.4.2 point 2. (Performing Connectivity
     * Checks) of ICE spec (RFC8445)
     */
    priv_conn_check_unfreeze_next (agent);
    pair = priv_conn_check_find_next_waiting (stream->conncheck_list);
  }

  if (pair) {
    stun_sent = priv_conn_check_initiate (agent, pair);
    priv_print_conn_check_lists (agent, G_STRFUNC,
        ", initiated an ordinary connection check");
  }
  return stun_sent;
}

static gboolean
priv_conn_check_triggered_check (NiceAgent *agent, NiceStream *stream)
{
  CandidateCheckPair *pair;
  gboolean stun_sent = FALSE;

  /* step: perform a test from the triggered checks list,
   * sect 6.1.4.2 point 1. (Performing Connectivity Checks) of ICE
   * spec (RFC8445)
   */
  pair = priv_get_pair_from_triggered_check_queue (agent);

  if (pair) {
    stun_sent = priv_conn_check_initiate (agent, pair);
    priv_print_conn_check_lists (agent, G_STRFUNC,
        ", initiated a connection check from triggered check list");
  }
  return stun_sent;
}


static gboolean
priv_conn_check_tick_stream_nominate (NiceAgent *agent, NiceStream *stream)
{
  gboolean keep_timer_going = FALSE;
  /* s_xxx counters are stream-wide */
  guint s_inprogress = 0;
  guint s_succeeded = 0;
  guint s_discovered = 0;
  guint s_nominated = 0;
  guint s_waiting_for_nomination = 0;
  guint s_valid = 0;
  guint s_frozen = 0;
  guint s_waiting = 0;
  CandidateCheckPair *other_stream_pair = NULL;
  GSList *i, *j;

  /* Search for a nominated pair (or selected to be nominated pair)
   * from another stream.
   */
  for (i = agent->streams; i ; i = i->next) {
    NiceStream *s = i->data;
    if (s->id == stream->id)
      continue;
    for (j = s->conncheck_list; j ; j = j->next) {
      CandidateCheckPair *p = j->data;
      if (p->nominated || (p->use_candidate_on_next_check &&
          p->state != NICE_CHECK_FAILED)) {
        other_stream_pair = p;
        break;
      }
    }
    if (other_stream_pair)
      break;
  }

  /* we compute some stream-wide counter values */
  for (i = stream->conncheck_list; i ; i = i->next) {
    CandidateCheckPair *p = i->data;
    if (p->state == NICE_CHECK_FROZEN)
      s_frozen++;
    else if (p->state == NICE_CHECK_IN_PROGRESS)
      s_inprogress++;
    else if (p->state == NICE_CHECK_WAITING)
      s_waiting++;
    else if (p->state == NICE_CHECK_SUCCEEDED)
      s_succeeded++;
    else if (p->state == NICE_CHECK_DISCOVERED)
      s_discovered++;
    if (p->valid)
      s_valid++;

    if ((p->state == NICE_CHECK_SUCCEEDED || p->state == NICE_CHECK_DISCOVERED)
        && p->nominated)
      s_nominated++;
    else if ((p->state == NICE_CHECK_SUCCEEDED ||
            p->state == NICE_CHECK_DISCOVERED) && !p->nominated)
      s_waiting_for_nomination++;
  }

  /* note: keep the timer going as long as there is work to be done */
  if (s_inprogress)
    keep_timer_going = TRUE;
  
  if (s_nominated < stream->n_components &&
      s_waiting_for_nomination) {
    if (NICE_AGENT_IS_COMPATIBLE_WITH_RFC5245_OR_OC2007R2 (agent)) {
      if (agent->nomination_mode == NICE_NOMINATION_MODE_REGULAR &&
          agent->controlling_mode) {
#define NICE_MIN_NUMBER_OF_VALID_PAIRS 2
        /* ICE 8.1.1.1 Regular nomination
         * we choose to nominate the valid pair of a component if
         * - there is no pair left frozen, waiting or in-progress, or
         * - if there are at least two valid pairs, or
         * - if there is at least one valid pair of type HOST-HOST
         *
         * This is the "stopping criterion" described in 8.1.1.1, and is
         * a "local optimization" between accumulating more valid pairs,
         * and limiting the time spent waiting for in-progress connections
         * checks until they finally fail.
         */
        for (i = stream->components; i; i = i->next) {
          NiceComponent *component = i->data;
          CandidateCheckPair *other_component_pair = NULL;
          CandidateCheckPair *this_component_pair = NULL;
          NiceCandidate *lcand1 = NULL;
          NiceCandidate *rcand1 = NULL;
          NiceCandidate *lcand2, *rcand2;
          gboolean already_done = FALSE;
          gboolean found_other_component_pair = FALSE;
          gboolean found_other_stream_pair = FALSE;
          gboolean first_nomination = FALSE;
          gboolean stopping_criterion;
          /* p_xxx counters are component-wide */
          guint p_valid = 0;
          guint p_frozen = 0;
          guint p_waiting = 0;
          guint p_inprogress = 0;
          guint p_host_host_valid = 0;

          /* we compute some component-wide counter values */
          for (j = stream->conncheck_list; j ; j = j->next) {
            CandidateCheckPair *p = j->data;
            if (p->component_id == component->id) {
              /* verify that the choice of the pair to be nominated
               * has not already been done
               */
              if (p->use_candidate_on_next_check)
                already_done = TRUE;
              if (p->state == NICE_CHECK_FROZEN)
                p_frozen++;
              else if (p->state == NICE_CHECK_WAITING)
                p_waiting++;
              else if (p->state == NICE_CHECK_IN_PROGRESS)
                p_inprogress++;
              if (p->valid)
                p_valid++;
              if (p->valid &&
                  p->local->type == NICE_CANDIDATE_TYPE_HOST &&
                  p->remote->type == NICE_CANDIDATE_TYPE_HOST)
                p_host_host_valid++;
            }
          }

          if (already_done)
            continue;

          /* Search for a nominated pair (or selected to be nominated pair)
           * from another component of this stream.
           */
          for (j = stream->conncheck_list; j ; j = j->next) {
            CandidateCheckPair *p = j->data;
            if (p->component_id == component->id)
              continue;
            if (p->nominated || (p->use_candidate_on_next_check &&
                p->state != NICE_CHECK_FAILED)) {
              other_component_pair = p;
              break;
            }
          }

          if (other_stream_pair == NULL && other_component_pair == NULL)
            first_nomination = TRUE;

          /* We choose a pair to be nominated in the list of valid
           * pairs.
           *
           * this pair will be the one with the highest priority,
           * when we don't have other nominated pairs in other
           * components and in other streams
           *
           * this pair will be a pair compatible with another nominated
           * pair from another component if we found one.
           *
           * else this pair will be a pair compatible with another
           * nominated pair from another stream if we found one.
           *
           */
          for (j = stream->conncheck_list; j ; j = j->next) {
            CandidateCheckPair *p = j->data;
            /* note: highest priority item selected (list always sorted) */
            if (p->component_id == component->id &&
                !p->nominated &&
                !p->use_candidate_on_next_check &&
                p->valid) {
              /* According a ICE spec, sect 8.1.1.1.  "Regular
               * Nomination", we enqueue the check that produced this
               * valid pair. When this pair has been discovered, we want
               * to test its parent pair instead.
               */
              if (p->succeeded_pair != NULL) {
                g_assert_cmpint (p->state, ==, NICE_CHECK_DISCOVERED);
                p = p->succeeded_pair;
              }
              g_assert_cmpint (p->state, ==, NICE_CHECK_SUCCEEDED);

              if (this_component_pair == NULL)
                /* highest priority pair */
                this_component_pair = p;

              lcand1 = p->local;
              rcand1 = p->remote;

              if (first_nomination)
                /* use the highest priority pair */
                break;

              if (other_component_pair) {
                lcand2 = other_component_pair->local;
                rcand2 = other_component_pair->remote;
              }
              if (other_component_pair &&
                  lcand1->transport == lcand2->transport &&
                  nice_address_equal_no_port (&lcand1->addr, &lcand2->addr) &&
                  nice_address_equal_no_port (&rcand1->addr, &rcand2->addr)) {
                /* else continue the research with lower priority
                 * pairs, compatible with a nominated pair of
                 * another component
                 */
                this_component_pair = p;
                found_other_component_pair = TRUE;
                break;
              }

              if (other_stream_pair) {
                lcand2 = other_stream_pair->local;
                rcand2 = other_stream_pair->remote;
              }
              if (other_stream_pair &&
                  other_component_pair == NULL &&
                  lcand1->transport == lcand2->transport &&
                  nice_address_equal_no_port (&lcand1->addr, &lcand2->addr) &&
                  nice_address_equal_no_port (&rcand1->addr, &rcand2->addr)) {
                /* else continue the research with lower priority
                 * pairs, compatible with a nominated pair of
                 * another stream
                 */
                this_component_pair = p;
                found_other_stream_pair = TRUE;
                break;
              }
            }
          }

          /* No valid pair for this component */
          if (this_component_pair == NULL)
            continue;

          /* The stopping criterion tries to select a set of pairs of
           * the same kind (transport/type) for all components of a
           * stream, and for all streams, when possible (see last
           * paragraph).
           *
           * When no stream has nominated a pair yet, we apply the
           * following criterion :
           *   - stop if we have a valid host-host pair
           *   - or stop if we have at least "some* (2 in the current
           *     implementation) valid pairs, and select the best one
           *   - or stop if the conncheck cannot evolve more
           *
           * Else when the stream has a nominated pair in another
           * component we apply this criterion:
           *   - stop if we have a valid pair of the same kind than this
           *     other nominated pair.
           *   - or stop if the conncheck cannot evolve more
           *
           * Else when another stream has a nominated pair we apply the
           * following criterion:
           *   - stop if we have a valid pair of the same kind than the
           *     other nominated pair.
           *   - or stop if the conncheck cannot evolve more
           *
           * When no further evolution of the conncheck is possible, we
           * prefer to select the best valid pair we have, *even* if it
           * is not compatible with the transport of another stream of
           * component. We think it's still a better choice than marking
           * this component 'failed'.
           */
          stopping_criterion = FALSE;
          if (first_nomination && p_host_host_valid > 0) {
            stopping_criterion = TRUE;
            nice_debug ("Agent %p : stopping criterion: "
                "valid host-host pair", agent);
          } else if (first_nomination &&
              p_valid >= NICE_MIN_NUMBER_OF_VALID_PAIRS) {
            stopping_criterion = TRUE;
            nice_debug ("Agent %p : stopping criterion: "
                "*some* valid pairs", agent);
          } else if (found_other_component_pair) {
            stopping_criterion = TRUE;
            nice_debug ("Agent %p : stopping criterion: "
                "matching pair in another component", agent);
          } else if (found_other_stream_pair) {
            stopping_criterion = TRUE;
            nice_debug ("Agent %p : stopping criterion: "
                "matching pair in another stream", agent);
          } else if (p_waiting == 0 && p_inprogress == 0 && p_frozen == 0) {
            stopping_criterion = TRUE;
            nice_debug ("Agent %p : stopping criterion: "
                "no more pairs to check", agent);
          }

          if (!stopping_criterion)
            continue;

          /* when the stopping criterion is reached, we add the
           * selected pair for this component to the triggered checks
           * list
           */
          nice_debug ("Agent %p : restarting check of %s:%s pair %p with "
              "USE-CANDIDATE attrib (regular nomination) for "
              "stream %d component %d", agent,
              priv_candidate_transport_to_string (
                  this_component_pair->local->transport),
              priv_candidate_transport_to_string (
                  this_component_pair->remote->transport),
              this_component_pair, stream->id, component->id);
          this_component_pair->use_candidate_on_next_check = TRUE;
          priv_add_pair_to_triggered_check_queue (agent, this_component_pair);
          keep_timer_going = TRUE;
        }
      }
    } else if (agent->controlling_mode) {
      for (i = stream->components; i; i = i->next) {
        NiceComponent *component = i->data;

	for (j = stream->conncheck_list; j ; j = j->next) {
	  CandidateCheckPair *p = j->data;
	  /* note: highest priority item selected (list always sorted) */
	  if (p->component_id == component->id &&
              (p->state == NICE_CHECK_SUCCEEDED ||
               p->state == NICE_CHECK_DISCOVERED)) {
	    nice_debug ("Agent %p : restarting check of pair %p as the "
                "nominated pair.", agent, p);
	    p->nominated = TRUE;
            conn_check_update_selected_pair (agent, component, p);
            priv_add_pair_to_triggered_check_queue (agent, p);
            keep_timer_going = TRUE;
	    break; /* move to the next component */
	  }
	}
      }
    }
  }
  if (stream->tick_counter++ % 50 == 0)
    nice_debug ("Agent %p : stream %u: timer tick #%u: %u frozen, "
        "%u in-progress, %u waiting, %u succeeded, %u discovered, "
        "%u nominated, %u waiting-for-nom, %u valid",
        agent, stream->id, stream->tick_counter,
        s_frozen, s_inprogress, s_waiting, s_succeeded, s_discovered,
        s_nominated, s_waiting_for_nomination, s_valid);

  return keep_timer_going;

}

static void
conn_check_stop (NiceAgent *agent)
{
  if (agent->conncheck_timer_source == NULL)
    return;

  g_source_destroy (agent->conncheck_timer_source);
  g_source_unref (agent->conncheck_timer_source);
  agent->conncheck_timer_source = NULL;
  agent->conncheck_ongoing_idle_delay = 0;
}


/*
 * Timer callback that handles initiating and managing connectivity
 * checks (paced by the Ta timer).
 *
 * This function is designed for the g_timeout_add() interface.
 *
 * @return will return FALSE when no more pending timers.
 */
static gboolean priv_conn_check_tick_agent_locked (NiceAgent *agent,
    gpointer user_data)
{
  gboolean keep_timer_going = FALSE;
  gboolean stun_sent = FALSE;
  GSList *i, *j;

  /* step: process triggered checks
   * these steps are ordered by priority, since a single stun request
   * is sent per callback, we process the important steps first.
   *
   * perform a single stun request per timer callback,
   * to respect stun pacing
   */
  for (i = agent->streams; i && !stun_sent; i = i->next) {
    NiceStream *stream = i->data;

    stun_sent = priv_conn_check_triggered_check (agent, stream);
  }

  /* step: process ongoing STUN transactions */
  for (i = agent->streams; i && !stun_sent; i = i->next) {
    NiceStream *stream = i->data;

    stun_sent = priv_conn_check_tick_stream (agent, stream);
  }

  /* step: process ordinary checks */
  for (i = agent->streams; i && !stun_sent; i = i->next) {
    NiceStream *stream = i->data;

    stun_sent = priv_conn_check_ordinary_check (agent, stream);
  }

  if (stun_sent)
    keep_timer_going = TRUE;

  /* step: try to nominate a pair
   */
  for (i = agent->streams; i; i = i->next) {
    NiceStream *stream = i->data;

    if (priv_conn_check_tick_stream_nominate (agent, stream))
      keep_timer_going = TRUE;
  }

  /* note: we provide a grace period before declaring a component as
   * failed. Components marked connected, and then ready follow another
   * code path, and are not concerned by this grace period.
   */
  if (!keep_timer_going && agent->conncheck_ongoing_idle_delay == 0)
    nice_debug ("Agent %p : waiting %d msecs before checking "
        "for failed components.", agent, agent->idle_timeout);

  if (keep_timer_going)
    agent->conncheck_ongoing_idle_delay = 0;
  else
    agent->conncheck_ongoing_idle_delay += agent->timer_ta;

  /* step: stop timer if no work left */
  if (!keep_timer_going &&
      agent->conncheck_ongoing_idle_delay >= agent->idle_timeout) {
    nice_debug ("Agent %p : checking for failed components now.", agent);
    for (i = agent->streams; i; i = i->next) {
      NiceStream *stream = i->data;
      priv_update_check_list_failed_components (agent, stream);
      for (j = stream->components; j; j = j->next) {
        NiceComponent *component = j->data;
        conn_check_update_check_list_state_for_ready (agent, stream, component);
      }
    }

    nice_debug ("Agent %p : %s: stopping conncheck timer", agent, G_STRFUNC);
    priv_print_conn_check_lists (agent, G_STRFUNC,
        ", conncheck timer stopped");

    /* Stopping the timer so destroy the source.. this will allow
       the timer to be reset if we get a set_remote_candidates after this
       point */
    conn_check_stop (agent);

    /* XXX: what to signal, is all processing now really done? */
    nice_debug ("Agent %p : changing conncheck state to COMPLETED.", agent);
    return FALSE;
  }

  return TRUE;
}

static gboolean priv_conn_keepalive_retransmissions_tick_agent_locked (
    NiceAgent *agent, gpointer pointer)
{
  CandidatePair *pair = (CandidatePair *) pointer;

  g_source_destroy (pair->keepalive.tick_source);
  g_source_unref (pair->keepalive.tick_source);
  pair->keepalive.tick_source = NULL;

  switch (stun_timer_refresh (&pair->keepalive.timer)) {
    case STUN_USAGE_TIMER_RETURN_TIMEOUT:
      {
        /* Time out */
        StunTransactionId id;
        NiceComponent *component;

        if (!agent_find_component (agent,
                pair->keepalive.stream_id, pair->keepalive.component_id,
                NULL, &component)) {
          nice_debug ("Could not find stream or component in"
              " priv_conn_keepalive_retransmissions_tick");
          return FALSE;
        }

        stun_message_id (&pair->keepalive.stun_message, id);
        stun_agent_forget_transaction (&component->stun_agent, id);
        pair->keepalive.stun_message.buffer = NULL;

        if (agent->media_after_tick) {
          nice_debug ("Agent %p : Keepalive conncheck timed out!! "
              "but media was received. Suspecting keepalive lost because of "
              "network bottleneck", agent);
        } else {
          nice_debug ("Agent %p : Keepalive conncheck timed out!! "
              "peer probably lost connection", agent);
          agent_signal_component_state_change (agent,
              pair->keepalive.stream_id, pair->keepalive.component_id,
              NICE_COMPONENT_STATE_FAILED);
        }
        break;
      }
    case STUN_USAGE_TIMER_RETURN_RETRANSMIT:
      /* Retransmit */
      agent_socket_send (pair->local->sockptr, &pair->remote->addr,
          stun_message_length (&pair->keepalive.stun_message),
          (gchar *)pair->keepalive.stun_buffer);

      nice_debug ("Agent %p : Retransmitting keepalive conncheck",
          agent);

      G_GNUC_FALLTHROUGH;
    case STUN_USAGE_TIMER_RETURN_SUCCESS:
      agent_timeout_add_with_context (agent,
          &pair->keepalive.tick_source,
          "Pair keepalive", stun_timer_remainder (&pair->keepalive.timer),
          priv_conn_keepalive_retransmissions_tick_agent_locked, pair);
      break;
    default:
      g_assert_not_reached();
      break;
  }

  return FALSE;
}

static guint32 peer_reflexive_candidate_priority (NiceAgent *agent,
    NiceCandidate *local_candidate)
{
  NiceCandidate *candidate_priority =
      nice_candidate_new (NICE_CANDIDATE_TYPE_PEER_REFLEXIVE);
  guint32 priority;

  candidate_priority->transport = local_candidate->transport;
  candidate_priority->component_id = local_candidate->component_id;
  candidate_priority->base_addr = local_candidate->addr;
  if (agent->compatibility == NICE_COMPATIBILITY_GOOGLE) {
    priority = nice_candidate_jingle_priority (candidate_priority);
  } else if (agent->compatibility == NICE_COMPATIBILITY_MSN ||
             agent->compatibility == NICE_COMPATIBILITY_OC2007) {
    priority = nice_candidate_msn_priority (candidate_priority);
  } else if (agent->compatibility == NICE_COMPATIBILITY_OC2007R2) {
    priority = nice_candidate_ms_ice_priority (candidate_priority,
        agent->reliable, FALSE);
  } else {
    priority = nice_candidate_ice_priority (candidate_priority,
        agent->reliable, FALSE);
  }
  nice_candidate_free (candidate_priority);

  return priority;
}

/* Returns the priority of a local candidate of type peer-reflexive that
 * would be learned as a consequence of a check from this local
 * candidate. See RFC 5245, section 7.1.2.1. "PRIORITY and USE-CANDIDATE".
 * RFC 5245 is more explanatory than RFC 8445 on this detail.
 *
 * Apply to local candidates of type host only, because candidates of type
 * relay are supposed to have a public IP address, that wont generate
 * a peer-reflexive address. Server-reflexive candidates are not
 * concerned too, because no STUN request is sent with a local candidate
 * of this type.
 */
static guint32 stun_request_priority (NiceAgent *agent,
    NiceCandidate *local_candidate)
{
  if (local_candidate->type == NICE_CANDIDATE_TYPE_HOST)
    return peer_reflexive_candidate_priority (agent, local_candidate);
  else
    return local_candidate->priority;
}

static void ms_ice2_legacy_conncheck_send(StunMessage *msg, NiceSocket *sock,
    const NiceAddress *remote_addr)
{
  uint32_t *fingerprint_attr;
  uint32_t fingerprint_orig;
  uint16_t fingerprint_len;
  size_t buffer_len;

  if (msg->agent->ms_ice2_send_legacy_connchecks == FALSE) {
    return;
  }

  fingerprint_attr = (uint32_t *)stun_message_find (msg,
      STUN_ATTRIBUTE_FINGERPRINT, &fingerprint_len);

  if (fingerprint_attr == NULL) {
    nice_debug ("FINGERPRINT not found.");
    return;
  }

  if (fingerprint_len != sizeof (fingerprint_orig)) {
    nice_debug ("Unexpected FINGERPRINT length %u.", fingerprint_len);
    return;
  }

  memcpy (&fingerprint_orig, fingerprint_attr, sizeof (fingerprint_orig));

  buffer_len = stun_message_length (msg);

  *fingerprint_attr = stun_fingerprint (msg->buffer, buffer_len, TRUE);

  agent_socket_send (sock, remote_addr, buffer_len, (gchar *)msg->buffer);

  memcpy (fingerprint_attr, &fingerprint_orig, sizeof (fingerprint_orig));
}

/*
 * Timer callback that handles initiating and managing connectivity
 * checks (paced by the Ta timer).
 *
 * This function is designed for the g_timeout_add() interface.
 *
 * @return will return FALSE when no more pending timers.
 */
static gboolean priv_conn_keepalive_tick_unlocked (NiceAgent *agent)
{
  GSList *i, *j, *k;
  int errors = 0;
  size_t buf_len = 0;
  guint64 now;
  guint64 min_next_tick;
  guint64 next_timer_tick;

  now = g_get_monotonic_time ();
  min_next_tick = now + 1000 * NICE_AGENT_TIMER_TR_DEFAULT;

  /* case 1: session established and media flowing
   *         (ref ICE sect 11 "Keepalives" RFC-8445)
   * TODO: keepalives should be send only when no packet has been sent
   * on that pair in the last Tr seconds, and not unconditionally.
   */
  for (i = agent->streams; i; i = i->next) {

    NiceStream *stream = i->data;
    for (j = stream->components; j; j = j->next) {
      NiceComponent *component = j->data;
      if (component->selected_pair.local != NULL) {
	CandidatePair *p = &component->selected_pair;

        /* Disable keepalive checks on TCP candidates unless explicitly enabled */
        if (p->local->transport != NICE_CANDIDATE_TRANSPORT_UDP &&
            !agent->keepalive_conncheck)
          continue;

        if (p->keepalive.next_tick) {
          if (p->keepalive.next_tick < min_next_tick)
            min_next_tick = p->keepalive.next_tick;
          if (now < p->keepalive.next_tick)
            continue;
        }

        if (agent->compatibility == NICE_COMPATIBILITY_GOOGLE ||
            agent->keepalive_conncheck) {
          uint8_t uname[NICE_STREAM_MAX_UNAME];
          size_t uname_len =
              priv_create_username (agent, agent_find_stream (agent, stream->id),
                  component->id, p->remote, p->local, uname, sizeof (uname),
                  FALSE);
          uint8_t *password = NULL;
          size_t password_len = priv_get_password (agent,
              agent_find_stream (agent, stream->id), p->remote, &password);

          if (p->keepalive.stun_message.buffer != NULL) {
            nice_debug ("Agent %p: Keepalive for s%u:c%u still"
                " retransmitting, not restarting", agent, stream->id,
                component->id);
            continue;
          }

          if (nice_debug_is_enabled ()) {
            gchar tmpbuf[INET6_ADDRSTRLEN];
            nice_address_to_string (&p->remote->addr, tmpbuf);
            nice_debug ("Agent %p : Keepalive STUN-CC REQ to '%s:%u', "
                "(c-id:%u), username='%.*s' (%" G_GSIZE_FORMAT "), "
                "password='%.*s' (%" G_GSIZE_FORMAT "), priority=%08x.",
                agent, tmpbuf, nice_address_get_port (&p->remote->addr),
                component->id, (int) uname_len, uname, uname_len,
                (int) password_len, password, password_len,
                p->stun_priority);
          }
          if (uname_len > 0) {
            buf_len = stun_usage_ice_conncheck_create (&component->stun_agent,
                &p->keepalive.stun_message, p->keepalive.stun_buffer,
                sizeof(p->keepalive.stun_buffer),
                uname, uname_len, password, password_len,
                agent->controlling_mode, agent->controlling_mode,
                p->stun_priority,
                agent->tie_breaker,
                NULL,
                agent_to_ice_compatibility (agent));

            nice_debug ("Agent %p: conncheck created %zd - %p",
                agent, buf_len, p->keepalive.stun_message.buffer);

            if (buf_len > 0) {
              stun_timer_start (&p->keepalive.timer,
                  agent->stun_initial_timeout,
                  agent->stun_max_retransmissions);

              agent->media_after_tick = FALSE;

              /* send the conncheck */
              agent_socket_send (p->local->sockptr, &p->remote->addr,
                  buf_len, (gchar *)p->keepalive.stun_buffer);

              p->keepalive.stream_id = stream->id;
              p->keepalive.component_id = component->id;
              p->keepalive.next_tick = now + 1000 * NICE_AGENT_TIMER_TR_DEFAULT;

              agent_timeout_add_with_context (agent,
                  &p->keepalive.tick_source, "Pair keepalive",
                  stun_timer_remainder (&p->keepalive.timer),
                  priv_conn_keepalive_retransmissions_tick_agent_locked, p);

              next_timer_tick = now + agent->timer_ta * 1000;
              goto done;
            } else {
              ++errors;
            }
          }
        } else {
          buf_len = stun_usage_bind_keepalive (&component->stun_agent,
              &p->keepalive.stun_message, p->keepalive.stun_buffer,
              sizeof(p->keepalive.stun_buffer));

          if (buf_len > 0) {
            agent_socket_send (p->local->sockptr, &p->remote->addr, buf_len,
                (gchar *)p->keepalive.stun_buffer);

            p->keepalive.next_tick = now + 1000 * NICE_AGENT_TIMER_TR_DEFAULT;

            if (agent->compatibility == NICE_COMPATIBILITY_OC2007R2) {
              ms_ice2_legacy_conncheck_send (&p->keepalive.stun_message,
                  p->local->sockptr, &p->remote->addr);
            }

            if (nice_debug_is_enabled ()) {
              gchar tmpbuf[INET6_ADDRSTRLEN];
              nice_address_to_string (&p->local->base_addr, tmpbuf);
              nice_debug ("Agent %p : resending STUN to keep the "
                  "selected base address %s:%u alive in s%d/c%d.", agent,
                  tmpbuf, nice_address_get_port (&p->local->base_addr),
                  stream->id, component->id);
            }

            next_timer_tick = now + agent->timer_ta * 1000;
            goto done;
          } else {
            ++errors;
          }
        }
      }
    }
  }

  /* case 2: connectivity establishment ongoing
   *         (ref ICE sect 5.1.1.4 "Keeping Candidates Alive" RFC-8445)
   */
  for (i = agent->streams; i; i = i->next) {
    NiceStream *stream = i->data;
    for (j = stream->components; j; j = j->next) {
      NiceComponent *component = j->data;
      if (component->state < NICE_COMPONENT_STATE_CONNECTED &&
          agent->stun_server_ip) {
        NiceAddress stun_server;
        if (nice_address_set_from_string (&stun_server, agent->stun_server_ip)) {
          StunAgent stun_agent;
          uint8_t stun_buffer[STUN_MAX_MESSAGE_SIZE_IPV6];
          StunMessage stun_message;
          size_t buffer_len = 0;

          nice_address_set_port (&stun_server, agent->stun_server_port);

          nice_agent_init_stun_agent (agent, &stun_agent);

          buffer_len = stun_usage_bind_create (&stun_agent,
              &stun_message, stun_buffer, sizeof(stun_buffer));

          for (k = component->local_candidates; k; k = k->next) {
            NiceCandidate *candidate = (NiceCandidate *) k->data;
            if (candidate->type == NICE_CANDIDATE_TYPE_HOST &&
                candidate->transport == NICE_CANDIDATE_TRANSPORT_UDP &&
                nice_address_ip_version (&candidate->addr) ==
                nice_address_ip_version (&stun_server)) {

              if (candidate->keepalive_next_tick) {
                if (candidate->keepalive_next_tick < min_next_tick)
                  min_next_tick = candidate->keepalive_next_tick;
                if (now < candidate->keepalive_next_tick)
                continue;
              }

              /* send the conncheck */
              if (nice_debug_is_enabled ()) {
                gchar tmpbuf[INET6_ADDRSTRLEN];
                nice_address_to_string (&candidate->addr, tmpbuf);
                nice_debug ("Agent %p : resending STUN to keep the local "
                    "candidate %s:%u alive in s%d/c%d.", agent,
                    tmpbuf, nice_address_get_port (&candidate->addr),
                    stream->id, component->id);
              }
              agent_socket_send (candidate->sockptr, &stun_server,
                  buffer_len, (gchar *)stun_buffer);
              candidate->keepalive_next_tick = now +
                  1000 * NICE_AGENT_TIMER_TR_DEFAULT;
              next_timer_tick = now + agent->timer_ta * 1000;
              goto done;
            }
          }
        }
      }
    }
  }

  next_timer_tick = min_next_tick;

  done:
  if (errors) {
    nice_debug ("Agent %p : %s: stopping keepalive timer", agent, G_STRFUNC);
    return FALSE;
  }

  if (agent->keepalive_timer_source) {
    g_source_destroy (agent->keepalive_timer_source);
    g_source_unref (agent->keepalive_timer_source);
    agent->keepalive_timer_source = NULL;
  }
  agent_timeout_add_with_context (agent, &agent->keepalive_timer_source,
      "Connectivity keepalive timeout", (next_timer_tick - now)/ 1000,
      priv_conn_keepalive_tick_agent_locked, NULL);
  return TRUE;
}

static gboolean priv_conn_keepalive_tick_agent_locked (NiceAgent *agent,
    gpointer pointer)
{
  gboolean ret;

  ret = priv_conn_keepalive_tick_unlocked (agent);
  if (ret == FALSE) {
    if (agent->keepalive_timer_source) {
      g_source_destroy (agent->keepalive_timer_source);
      g_source_unref (agent->keepalive_timer_source);
      agent->keepalive_timer_source = NULL;
    }
  }

  return ret;
}


static gboolean priv_turn_allocate_refresh_retransmissions_tick_agent_locked (
    NiceAgent *agent, gpointer pointer)
{
  CandidateRefresh *cand = (CandidateRefresh *) pointer;

  g_source_destroy (cand->tick_source);
  g_source_unref (cand->tick_source);
  cand->tick_source = NULL;

  switch (stun_timer_refresh (&cand->timer)) {
    case STUN_USAGE_TIMER_RETURN_TIMEOUT:
      {
        /* Time out */
        StunTransactionId id;

        stun_message_id (&cand->stun_message, id);
        stun_agent_forget_transaction (&cand->stun_agent, id);

        refresh_free (agent, cand);
        break;
      }
    case STUN_USAGE_TIMER_RETURN_RETRANSMIT:
      /* Retransmit */
      agent_socket_send (cand->nicesock, &cand->server,
          stun_message_length (&cand->stun_message), (gchar *)cand->stun_buffer);

      G_GNUC_FALLTHROUGH;
    case STUN_USAGE_TIMER_RETURN_SUCCESS:
      agent_timeout_add_with_context (agent, &cand->tick_source,
          "Candidate TURN refresh", stun_timer_remainder (&cand->timer),
          priv_turn_allocate_refresh_retransmissions_tick_agent_locked, cand);
      break;
    default:
      /* Nothing to do. */
      break;
  }

  return G_SOURCE_REMOVE;
}

static void priv_turn_allocate_refresh_tick_unlocked (NiceAgent *agent,
    CandidateRefresh *cand)
{
  uint8_t *username;
  gsize username_len;
  uint8_t *password;
  gsize password_len;
  size_t buffer_len = 0;
  StunUsageTurnCompatibility turn_compat =
      agent_to_turn_compatibility (agent);

  username = (uint8_t *)cand->candidate->turn->username;
  username_len = (size_t) strlen (cand->candidate->turn->username);
  password = (uint8_t *)cand->candidate->turn->password;
  password_len = (size_t) strlen (cand->candidate->turn->password);

  if (turn_compat == STUN_USAGE_TURN_COMPATIBILITY_MSN ||
      turn_compat == STUN_USAGE_TURN_COMPATIBILITY_OC2007) {
    username = cand->candidate->turn->decoded_username;
    password = cand->candidate->turn->decoded_password;
    username_len = cand->candidate->turn->decoded_username_len;
    password_len = cand->candidate->turn->decoded_password_len;
  }

  buffer_len = stun_usage_turn_create_refresh (&cand->stun_agent,
      &cand->stun_message,  cand->stun_buffer, sizeof(cand->stun_buffer),
      cand->stun_resp_msg.buffer == NULL ? NULL : &cand->stun_resp_msg, -1,
      username, username_len,
      password, password_len,
      turn_compat);

  nice_debug ("Agent %p : Sending allocate Refresh %zd", agent,
      buffer_len);

  if (cand->tick_source != NULL) {
    g_source_destroy (cand->tick_source);
    g_source_unref (cand->tick_source);
    cand->tick_source = NULL;
  }

  if (buffer_len > 0) {
    stun_timer_start (&cand->timer,
        agent->stun_initial_timeout,
        agent->stun_max_retransmissions);

    /* send the refresh */
    agent_socket_send (cand->nicesock, &cand->server,
        buffer_len, (gchar *)cand->stun_buffer);

    agent_timeout_add_with_context (agent, &cand->tick_source,
        "Candidate TURN refresh", stun_timer_remainder (&cand->timer),
        priv_turn_allocate_refresh_retransmissions_tick_agent_locked, cand);
  }

}


/*
 * Timer callback that handles refreshing TURN allocations
 *
 * This function is designed for the g_timeout_add() interface.
 *
 * @return will return FALSE when no more pending timers.
 */
static gboolean priv_turn_allocate_refresh_tick_agent_locked (NiceAgent *agent,
    gpointer pointer)
{
  CandidateRefresh *cand = (CandidateRefresh *) pointer;

  priv_turn_allocate_refresh_tick_unlocked (agent, cand);

  return G_SOURCE_REMOVE;
}


/*
 * Initiates the next pending connectivity check.
 */
void conn_check_schedule_next (NiceAgent *agent)
{
  if (agent->discovery_unsched_items > 0)
    nice_debug ("Agent %p : WARN: starting conn checks before local candidate gathering is finished.", agent);

  /* step: schedule timer if not running yet */
  if (agent->conncheck_timer_source == NULL) {
    agent_timeout_add_with_context (agent, &agent->conncheck_timer_source,
        "Connectivity check schedule", agent->timer_ta,
        priv_conn_check_tick_agent_locked, NULL);
  }

  /* step: also start the keepalive timer */
  if (agent->keepalive_timer_source == NULL) {
    agent_timeout_add_with_context (agent, &agent->keepalive_timer_source,
        "Connectivity keepalive timeout", agent->timer_ta,
        priv_conn_keepalive_tick_agent_locked, NULL);
  }
}

/*
 * Compares two connectivity check items. Checkpairs are sorted
 * in descending priority order, with highest priority item at
 * the start of the list.
 */
gint conn_check_compare (const CandidateCheckPair *a, const CandidateCheckPair *b)
{
  if (a->priority > b->priority)
    return -1;
  else if (a->priority < b->priority)
    return 1;
  return 0;
}

/* Find a transport compatible with a given socket.
 *
 * Returns TRUE when a matching transport can be guessed from
 * the type of the socket in an unambiguous way.
 */
static gboolean
nice_socket_has_compatible_transport (NiceSocket *socket,
    NiceCandidateTransport *transport)
{
  gboolean found = TRUE;

  g_assert (socket);
  g_assert (transport);

  switch (socket->type) {
    case NICE_SOCKET_TYPE_TCP_BSD:
      if (nice_tcp_bsd_socket_get_passive_parent (socket))
        *transport = NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE;
      else
        *transport = NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE;
      break;
    case NICE_SOCKET_TYPE_TCP_PASSIVE:
      *transport = NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE;
      break;
    case NICE_SOCKET_TYPE_TCP_ACTIVE:
      *transport = NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE;
      break;
    case NICE_SOCKET_TYPE_UDP_BSD:
      *transport = NICE_CANDIDATE_TRANSPORT_UDP;
      break;
    default:
      found = FALSE;
  }

  return found;
}

/* Test if a local socket and a local candidate are compatible. This
 * function does supplementary tests when the address and port are not
 * sufficient to give a unique candidate. We try to avoid comparing
 * directly the sockptr value, when possible, to rely on objective
 * properties of the candidate and the socket instead, and we also
 * choose to ignore the conncheck list for the same reason.
 */
static gboolean
local_candidate_and_socket_compatible (NiceAgent *agent,
    NiceCandidate *lcand, NiceSocket *socket)
{
  gboolean ret = TRUE;
  NiceCandidateTransport transport;

  g_assert (socket);
  g_assert (lcand);

  if (nice_socket_has_compatible_transport (socket, &transport)) {
    ret = (lcand->transport == transport);
    /* tcp-active discovered peer-reflexive local candidate, where
     * socket is the tcp connect related socket */
    if (ret && transport == NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE &&
        nice_address_get_port (&lcand->addr) > 0)
      ret = (lcand->sockptr == socket);
  } else if (socket->type == NICE_SOCKET_TYPE_UDP_TURN)
    /* Socket of type udp-turn will match a unique local candidate
     * by its sockptr value. An an udp-turn socket doesn't carry enough
     * information when base socket is udp-turn-over-tcp to disambiguate
     * between a tcp-act and a tcp-pass local candidate.
     */
    ret = (lcand->sockptr == socket);

  return ret;
}

/* Test if a local socket and a remote candidate are compatible.
 * This function is very close to its local candidate counterpart,
 * the difference is that we also use information from the local
 * candidate we may have identified previously. This is needed
 * to disambiguate the transport of the candidate with a socket
 * of type udp-turn.
 *
 */
static gboolean
remote_candidate_and_socket_compatible (NiceAgent *agent,
    NiceCandidate *lcand, NiceCandidate *rcand, NiceSocket *socket)
{
  gboolean ret = TRUE;
  NiceCandidateTransport transport;

  g_assert (socket);
  g_assert (rcand);

  if (nice_socket_has_compatible_transport (socket, &transport))
    ret = (conn_check_match_transport (rcand->transport) == transport);

  /* This supplementary test with the local candidate is needed with
   * socket of type udp-turn, the type doesn't allow to disambiguate
   * between a tcp-pass and a tcp-act remote candidate
   */
  if (lcand && ret)
    ret = (conn_check_match_transport (lcand->transport) == rcand->transport);

  return ret;
}

void
conn_check_remote_candidates_set(NiceAgent *agent, NiceStream *stream,
    NiceComponent *component)
{
  GList *i;
  GSList *j;
  NiceCandidate *lcand = NULL, *rcand = NULL;

  nice_debug ("Agent %p : conn_check_remote_candidates_set %u %u",
    agent, stream->id, component->id);

  if (stream->remote_ufrag[0] == 0)
    return;

  if (component->incoming_checks.head)
    nice_debug ("Agent %p : credentials have been set, "
      "we can process incoming checks", agent);

  for (i = component->incoming_checks.head; i;) {
    IncomingCheck *icheck = i->data;
    GList *i_next = i->next;

    nice_debug ("Agent %p : replaying icheck=%p (sock=%p)",
        agent, icheck, icheck->local_socket);

    /* sect 7.2.1.3., "Learning Peer Reflexive Candidates", has to
     * be handled separately */
    for (j = component->local_candidates; j; j = j->next) {
      NiceCandidate *cand = j->data;
      NiceAddress *addr;

      if (cand->type == NICE_CANDIDATE_TYPE_RELAYED)
        addr = &cand->addr;
      else
        addr = &cand->base_addr;

      if (nice_address_equal (&icheck->local_socket->addr, addr) &&
          local_candidate_and_socket_compatible (agent, cand,
          icheck->local_socket)) {
        lcand = cand;
        break;
      }
    }

    if (lcand == NULL) {
      for (j = component->local_candidates; j; j = j->next) {
        NiceCandidate *cand = j->data;
        NiceAddress *addr = &cand->base_addr;

        /* tcp-active (not peer-reflexive discovered) local candidate, where
         * socket is the tcp connect related socket */
        if (nice_address_equal_no_port (&icheck->local_socket->addr, addr) &&
            nice_address_get_port (&cand->addr) == 0 &&
            cand->transport == NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE &&
            local_candidate_and_socket_compatible (agent, cand,
            icheck->local_socket)) {
          lcand = cand;
          break;
        }
      }
    }

    g_assert (lcand != NULL);

    for (j = component->remote_candidates; j; j = j->next) {
      NiceCandidate *cand = j->data;
      if (nice_address_equal (&cand->addr, &icheck->from) &&
          remote_candidate_and_socket_compatible (agent, lcand, cand,
          icheck->local_socket)) {
        rcand = cand;
        break;
      }
    }

    if (lcand->transport == NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE) {
      CandidateCheckPair *pair = NULL;

      for (j = stream->conncheck_list; j; j = j->next) {
        CandidateCheckPair *p = j->data;
        if (lcand == p->local && rcand == p->remote) {
          pair = p;
          break;
        }
      }
      if (pair == NULL)
        priv_conn_check_add_for_candidate_pair_matched (agent,
            stream->id, component, lcand, rcand, NICE_CHECK_WAITING);
    }

    priv_schedule_triggered_check (agent, stream, component,
        icheck->local_socket, rcand);
    if (icheck->use_candidate)
      priv_mark_pair_nominated (agent, stream, component, lcand, rcand);

    if (icheck->username)
      g_free (icheck->username);
    g_slice_free (IncomingCheck, icheck);
    g_queue_delete_link (&component->incoming_checks, i);
    i = i_next;
  }
}

/*
 * Handle any processing steps for connectivity checks after
 * remote credentials have been set. This function handles
 * the special case where answerer has sent us connectivity
 * checks before the answer (containing credentials information),
 * reaches us. The special case is documented in RFC 5245 sect 7.2.
 * ).
 */
void conn_check_remote_credentials_set(NiceAgent *agent, NiceStream *stream)
{
  GSList *j;

  for (j = stream->components; j ; j = j->next) {
    NiceComponent *component = j->data;

    conn_check_remote_candidates_set(agent, stream, component);
  }
}

/*
 * Enforces the upper limit for connectivity checks by dropping
 * lower-priority pairs as described RFC 8445 section 6.1.2.5. See also
 * conn_check_add_for_candidate().
 * Returns TRUE if the pair in argument is one of the deleted pairs.
 */
static gboolean priv_limit_conn_check_list_size (NiceAgent *agent,
    NiceStream *stream, CandidateCheckPair *pair)
{
  guint valid = 0;
  guint cancelled = 0;
  gboolean deleted = FALSE;
  GSList *item = stream->conncheck_list;

  while (item) {
    CandidateCheckPair *p = item->data;
    GSList *next = item->next;

    valid++;
    /* We remove lower-priority pairs, but only the ones that can be
     * safely discarded without breaking an ongoing conncheck process.
     * This only includes pairs that are in the frozen state (those
     * initially added when remote candidates are received) or in failed
     * state. Pairs in any other state play a role in the conncheck, and
     * there removal may lead to a failing conncheck that would succeed
     * otherwise.
     *
     * We also remove failed pairs from the list unconditionally.
     */
    if ((valid > agent->max_conn_checks && p->state == NICE_CHECK_FROZEN) ||
        p->state == NICE_CHECK_FAILED) {
      if (p == pair)
        deleted = TRUE;
      nice_debug ("Agent %p : pair %p removed.", agent, p);
      candidate_check_pair_free (agent, p);
      stream->conncheck_list = g_slist_delete_link (stream->conncheck_list,
          item);
      cancelled++;
    }
    item = next;
  }

  if (cancelled > 0)
    nice_debug ("Agent %p : Pruned %d pairs. "
        "Conncheck list has %d elements left. "
        "Maximum connchecks allowed : %d", agent, cancelled,
        valid - cancelled, agent->max_conn_checks);

  return deleted;
}

/*
 * Changes the selected pair for the component if 'pair'
 * has higher priority than the currently selected pair. See
 * RFC 8445 sect 8.1.1. "Nominating Pairs"
 */
void
conn_check_update_selected_pair (NiceAgent *agent, NiceComponent *component,
    CandidateCheckPair *pair)
{
  CandidatePair cpair = { 0, };

  g_assert (component);
  g_assert (pair);
  /* pair is expected to have the nominated flag */
  g_assert (pair->nominated);
  if (pair->priority > component->selected_pair.priority) {
    gchar priority[NICE_CANDIDATE_PAIR_PRIORITY_MAX_SIZE];
    nice_candidate_pair_priority_to_string (pair->priority, priority);
    nice_debug ("Agent %p : changing SELECTED PAIR for component %u: %s:%s "
        "(prio:%s).", agent, component->id,
        pair->local->foundation, pair->remote->foundation, priority);

    cpair.local = pair->local;
    cpair.remote = pair->remote;
    cpair.priority = pair->priority;
    cpair.stun_priority = pair->stun_priority;

    nice_component_update_selected_pair (agent, component, &cpair);

    priv_conn_keepalive_tick_unlocked (agent);

    agent_signal_new_selected_pair (agent, pair->stream_id, component->id,
        pair->local, pair->remote);
  }
}

/*
 * Updates the check list state.
 *
 * Implements parts of the algorithm described in 
 * ICE sect 8.1.2. "Updating States" (RFC 5245): if for any
 * component, all checks have been completed and have
 * failed to produce a nominated pair, mark that component's
 * state to NICE_CHECK_FAILED.
 *
 * Sends a component state changesignal via 'agent'.
 */
static void priv_update_check_list_failed_components (NiceAgent *agent, NiceStream *stream)
{
  GSList *i;
  gboolean completed;
  guint nominated;
  /* note: emitting a signal might cause the client 
   *       to remove the stream, thus the component count
   *       must be fetched before entering the loop*/
  guint c, components = stream->n_components;

  if (stream->conncheck_list == NULL)
    return;

  for (i = agent->discovery_list; i; i = i->next) {
    CandidateDiscovery *d = i->data;

    /* There is still discovery ogoing for this stream,
     * so don't fail any of it's candidates.
     */
    if (d->stream_id == stream->id && !d->done)
      return;
  }
  if (agent->discovery_list != NULL)
    return;

  /* note: iterate the conncheck list for each component separately */
  for (c = 0; c < components; c++) {
    NiceComponent *component = NULL;
    if (!agent_find_component (agent, stream->id, c+1, NULL, &component))
      continue;

    nominated = 0;
    completed = TRUE;
    for (i = stream->conncheck_list; i; i = i->next) {
      CandidateCheckPair *p = i->data;

      g_assert_cmpuint (p->stream_id, ==, stream->id);

      if (p->component_id == (c + 1)) {
        if (p->nominated)
          ++nominated;
	if (p->state != NICE_CHECK_FAILED &&
            p->state != NICE_CHECK_SUCCEEDED &&
            p->state != NICE_CHECK_DISCOVERED)
	  completed = FALSE;
      }
    }
 
    /* note: all pairs are either failed or succeeded, and the component
     * has not produced a nominated pair.
     * Set the component to FAILED only if it actually had remote candidates
     * that failed.. */
    if (completed && nominated == 0 &&
        component != NULL && component->remote_candidates != NULL)
      agent_signal_component_state_change (agent,
					   stream->id,
					   (c + 1), /* component-id */
					   NICE_COMPONENT_STATE_FAILED);
  }
}

/*
 * Updates the check list state for a stream component.
 *
 * Implements the algorithm described in ICE sect 8.1.2 
 * "Updating States" (ID-19) as it applies to checks of 
 * a certain component. If there are any nominated pairs, 
 * ICE processing may be concluded, and component state is 
 * changed to READY.
 *
 * Sends a component state changesignal via 'agent'.
 */
void conn_check_update_check_list_state_for_ready (NiceAgent *agent,
    NiceStream *stream, NiceComponent *component)
{
  GSList *i;
  guint valid = 0, nominated = 0;

  g_assert (component);

  /* step: search for at least one nominated pair */
  for (i = stream->conncheck_list; i; i = i->next) {
    CandidateCheckPair *p = i->data;
    if (p->component_id == component->id) {
      if (p->valid) {
	++valid;
	if (p->nominated == TRUE) {
          ++nominated;
	}
      }
    }
  }

  if (nominated > 0) {
    /* Only go to READY if no checks are left in progress. If there are
     * any that are kept, then this function will be called again when the
     * conncheck tick timer finishes them all */
    if (priv_prune_pending_checks (agent, stream, component) == 0) {
      /* Continue through the states to give client code a nice
       * logical progression. See http://phabricator.freedesktop.org/D218 for
       * discussion. */
      if (component->state < NICE_COMPONENT_STATE_CONNECTING ||
          component->state == NICE_COMPONENT_STATE_FAILED)
        agent_signal_component_state_change (agent, stream->id, component->id,
            NICE_COMPONENT_STATE_CONNECTING);
      if (component->state < NICE_COMPONENT_STATE_CONNECTED)
        agent_signal_component_state_change (agent, stream->id, component->id,
            NICE_COMPONENT_STATE_CONNECTED);
      agent_signal_component_state_change (agent, stream->id,
          component->id, NICE_COMPONENT_STATE_READY);
    }
  }
  nice_debug ("Agent %p : conn.check list status: %u nominated, %u valid, c-id %u.", agent, nominated, valid, component->id);
}

/*
 * The remote party has signalled that the candidate pair
 * described by 'component' and 'remotecand' is nominated
 * for use.
 */
static void priv_mark_pair_nominated (NiceAgent *agent, NiceStream *stream, NiceComponent *component, NiceCandidate *localcand, NiceCandidate *remotecand)
{
  GSList *i;

  g_assert (component);

  if (NICE_AGENT_IS_COMPATIBLE_WITH_RFC5245_OR_OC2007R2 (agent) &&
      agent->controlling_mode)
    return;

  /* step: search for at least one nominated pair */
  for (i = stream->conncheck_list; i; i = i->next) {
    CandidateCheckPair *pair = i->data;
    if (pair->local == localcand && pair->remote == remotecand) {
      /* ICE, 7.2.1.5. Updating the Nominated Flag */
      /* note: TCP candidates typically produce peer reflexive
       * candidate, generating a "discovered" pair that can be
       * nominated.
       */
      if (pair->state == NICE_CHECK_SUCCEEDED &&
          pair->discovered_pair != NULL) {
        pair = pair->discovered_pair;
        g_assert_cmpint (pair->state, ==, NICE_CHECK_DISCOVERED);
      }

      /* If the received Binding request triggered a new check to be
       * enqueued in the triggered-check queue (Section 7.3.1.4), once
       * the check is sent and if it generates a successful response,
       * and generates a valid pair, the agent sets the nominated flag
       * of the pair to true
       */
      if (NICE_AGENT_IS_COMPATIBLE_WITH_RFC5245_OR_OC2007R2 (agent)) {
        if (g_slist_find (agent->triggered_check_queue, pair) ||
            pair->state == NICE_CHECK_IN_PROGRESS) {

        /* This pair is not always in the triggered check list, for
         * example if it is in-progress with a lower priority than an
         * already nominated pair.  Is that case, it is not rescheduled
         * for a connection check, see function
         * priv_schedule_triggered_check(), case NICE_CHECK_IN_PROGRESS.
         */
        pair->mark_nominated_on_response_arrival = TRUE;
        nice_debug ("Agent %p : pair %p (%s) is %s, "
            "will be nominated on response receipt.",
            agent, pair, pair->foundation,
            priv_state_to_string (pair->state));
        }
      }

      if (pair->valid ||
          !NICE_AGENT_IS_COMPATIBLE_WITH_RFC5245_OR_OC2007R2 (agent)) {
        nice_debug ("Agent %p : marking pair %p (%s) as nominated",
            agent, pair, pair->foundation);
        pair->nominated = TRUE;
      }

      if (pair->valid) {
        /* Do not step down to CONNECTED if we're already at state READY*/
        if (component->state == NICE_COMPONENT_STATE_FAILED)
          agent_signal_component_state_change (agent,
              stream->id, component->id, NICE_COMPONENT_STATE_CONNECTING);
        conn_check_update_selected_pair (agent, component, pair);
        if (component->state == NICE_COMPONENT_STATE_CONNECTING)
          /* step: notify the client of a new component state (must be done
           *       before the possible check list state update step */
          agent_signal_component_state_change (agent,
              stream->id, component->id, NICE_COMPONENT_STATE_CONNECTED);
      }

      if (pair->nominated)
        conn_check_update_check_list_state_for_ready (agent, stream, component);
    }
  }
}

/*
 * Creates a new connectivity check pair and adds it to
 * the agent's list of checks.
 */
static CandidateCheckPair *priv_add_new_check_pair (NiceAgent *agent,
    guint stream_id, NiceComponent *component, NiceCandidate *local,
    NiceCandidate *remote, NiceCheckState initial_state)
{
  NiceStream *stream;
  CandidateCheckPair *pair;
  guint64 priority;

  g_assert (local != NULL);
  g_assert (remote != NULL);

  priority = agent_candidate_pair_priority (agent, local, remote);

  if (component->selected_pair.priority &&
      priority < component->selected_pair.priority) {
    gchar prio1[NICE_CANDIDATE_PAIR_PRIORITY_MAX_SIZE];
    gchar prio2[NICE_CANDIDATE_PAIR_PRIORITY_MAX_SIZE];

    nice_candidate_pair_priority_to_string (priority, prio1);
    nice_candidate_pair_priority_to_string (component->selected_pair.priority,
        prio2);
    nice_debug ("Agent %p : do not create a pair that would have a priority "
        "%s lower than selected pair priority %s.", agent, prio1, prio2);
    return NULL;
  }

  stream = agent_find_stream (agent, stream_id);
  pair = g_slice_new0 (CandidateCheckPair);

  pair->stream_id = stream_id;
  pair->component_id = component->id;;
  pair->local = local;
  pair->remote = remote;
  /* note: we use the remote sockptr only in the case
   * of TCP transport
   */
  if (local->transport == NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE &&
      remote->type == NICE_CANDIDATE_TYPE_PEER_REFLEXIVE)
    pair->sockptr = (NiceSocket *) remote->sockptr;
  else
    pair->sockptr = (NiceSocket *) local->sockptr;
  g_snprintf (pair->foundation, NICE_CANDIDATE_PAIR_MAX_FOUNDATION, "%s:%s", local->foundation, remote->foundation);

  pair->priority = agent_candidate_pair_priority (agent, local, remote);
  nice_debug ("Agent %p : creating a new pair", agent);
  SET_PAIR_STATE (agent, pair, initial_state);
  {
      gchar tmpbuf1[INET6_ADDRSTRLEN];
      gchar tmpbuf2[INET6_ADDRSTRLEN];
      nice_address_to_string (&pair->local->addr, tmpbuf1);
      nice_address_to_string (&pair->remote->addr, tmpbuf2);
      nice_debug ("Agent %p : new pair %p : [%s]:%u --> [%s]:%u", agent, pair,
          tmpbuf1, nice_address_get_port (&pair->local->addr),
          tmpbuf2, nice_address_get_port (&pair->remote->addr));
  }
  pair->stun_priority = stun_request_priority (agent, local);

  stream->conncheck_list = g_slist_insert_sorted (stream->conncheck_list, pair,
      (GCompareFunc)conn_check_compare);

  nice_debug ("Agent %p : added a new pair %p with foundation '%s' and "
      "transport %s:%s to stream %u component %u",
      agent, pair, pair->foundation,
      priv_candidate_transport_to_string (pair->local->transport),
      priv_candidate_transport_to_string (pair->remote->transport),
      stream_id, component->id);

  if (initial_state == NICE_CHECK_FROZEN)
    priv_conn_check_unfreeze_maybe (agent, pair);

  /* implement the hard upper limit for number of
     checks (see sect 5.7.3 ICE ID-19): */
  if (agent->compatibility == NICE_COMPATIBILITY_RFC5245) {
    if (priv_limit_conn_check_list_size (agent, stream, pair))
      return NULL;
  }

  return pair;
}

NiceCandidateTransport
conn_check_match_transport (NiceCandidateTransport transport)
{
  switch (transport) {
    case NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE:
      return NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE;
      break;
    case NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE:
      return NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE;
      break;
    case NICE_CANDIDATE_TRANSPORT_TCP_SO:
    case NICE_CANDIDATE_TRANSPORT_UDP:
    default:
      return transport;
      break;
  }
}

static CandidateCheckPair *priv_conn_check_add_for_candidate_pair_matched (
    NiceAgent *agent, guint stream_id, NiceComponent *component,
     NiceCandidate *local, NiceCandidate *remote, NiceCheckState initial_state)
{
  CandidateCheckPair *pair;

  pair = priv_add_new_check_pair (agent, stream_id, component, local, remote,
      initial_state);
  if (pair) {
    if (component->state == NICE_COMPONENT_STATE_CONNECTED ||
        component->state == NICE_COMPONENT_STATE_READY) {
      agent_signal_component_state_change (agent,
          stream_id,
          component->id,
          NICE_COMPONENT_STATE_CONNECTED);
    } else {
      agent_signal_component_state_change (agent,
          stream_id,
          component->id,
          NICE_COMPONENT_STATE_CONNECTING);
    }
  }

  return pair;
}

gboolean conn_check_add_for_candidate_pair (NiceAgent *agent,
    guint stream_id, NiceComponent *component, NiceCandidate *local,
    NiceCandidate *remote)
{
  gboolean ret = FALSE;

  g_assert (local != NULL);
  g_assert (remote != NULL);

  /* note: do not create pairs where the local candidate is a srv-reflexive
   * or peer-reflexive (ICE 6.1.2.4. "Pruning the pairs" RFC 8445)
   */
  if ((agent->compatibility == NICE_COMPATIBILITY_RFC5245 ||
      agent->compatibility == NICE_COMPATIBILITY_WLM2009 ||
      agent->compatibility == NICE_COMPATIBILITY_OC2007R2) &&
      (local->type == NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE ||
      local->type == NICE_CANDIDATE_TYPE_PEER_REFLEXIVE)) {
    return FALSE;
  }

  /* note: do not create pairs where local candidate has TCP passive transport
   *       (ice-tcp-13 6.2. "Forming the Check Lists") */
  if (local->transport == NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE) {
    return FALSE;
  }

  /* note: match pairs only if transport and address family are the same */
  if (local->transport == conn_check_match_transport (remote->transport) &&
     local->addr.s.addr.sa_family == remote->addr.s.addr.sa_family) {
    if (priv_conn_check_add_for_candidate_pair_matched (agent, stream_id,
        component, local, remote, NICE_CHECK_FROZEN))
      ret = TRUE;
  }

  return ret;
}

/*
 * Forms new candidate pairs by matching the new remote candidate
 * 'remote_cand' with all existing local candidates of 'component'.
 * Implements the logic described in ICE sect 5.7.1. "Forming Candidate
 * Pairs" (ID-19).
 *
 * @param agent context
 * @param component pointer to the component
 * @param remote remote candidate to match with
 *
 * @return number of checks added, negative on fatal errors
 */
int conn_check_add_for_candidate (NiceAgent *agent, guint stream_id, NiceComponent *component, NiceCandidate *remote)
{
  GSList *i;
  int added = 0;
  int ret = 0;

  g_assert (remote != NULL);

  /* note: according to 7.2.1.3, "Learning Peer Reflexive Candidates",
   * the agent does not pair this candidate with any local candidates.
   */
  if (NICE_AGENT_IS_COMPATIBLE_WITH_RFC5245_OR_OC2007R2 (agent) &&
      remote->type == NICE_CANDIDATE_TYPE_PEER_REFLEXIVE)
  {
    return added;
  }

  for (i = component->local_candidates; i ; i = i->next) {
    NiceCandidate *local = i->data;

    if (agent->force_relay && local->type != NICE_CANDIDATE_TYPE_RELAYED)
        continue;

    ret = conn_check_add_for_candidate_pair (agent, stream_id, component, local, remote);

    if (ret) {
      ++added;
    }
  }

  return added;
}

/*
 * Forms new candidate pairs by matching the new local candidate
 * 'local_cand' with all existing remote candidates of 'component'.
 *
 * @param agent context
 * @param component pointer to the component
 * @param local local candidate to match with
 *
 * @return number of checks added, negative on fatal errors
 */
int conn_check_add_for_local_candidate (NiceAgent *agent, guint stream_id, NiceComponent *component, NiceCandidate *local)
{
  GSList *i;
  int added = 0;
  int ret = 0;

  g_assert (local != NULL);

  /*
   * note: according to 7.1.3.2.1 "Discovering Peer Reflexive
   * Candidates", the peer reflexive candidate is not paired
   * with other remote candidates
   */

  if (NICE_AGENT_IS_COMPATIBLE_WITH_RFC5245_OR_OC2007R2 (agent) &&
      local->type == NICE_CANDIDATE_TYPE_PEER_REFLEXIVE)
  {
    return added;
  }

  for (i = component->remote_candidates; i ; i = i->next) {

    NiceCandidate *remote = i->data;
    ret = conn_check_add_for_candidate_pair (agent, stream_id, component, local, remote);

    if (ret) {
      ++added;
    }
  }

  return added;
}

/*
 * Frees the CandidateCheckPair structure pointer to 
 * by 'user data'. Compatible with GDestroyNotify.
 */
static void candidate_check_pair_free (NiceAgent *agent,
    CandidateCheckPair *pair)
{
  priv_remove_pair_from_triggered_check_queue (agent, pair);
  priv_free_all_stun_transactions (pair, NULL);
  g_slice_free (CandidateCheckPair, pair);
}

/*
 * Frees all resources of all connectivity checks.
 */
void conn_check_free (NiceAgent *agent)
{
  GSList *i;
  for (i = agent->streams; i; i = i->next) {
    NiceStream *stream = i->data;

    if (stream->conncheck_list) {
      GSList *item;

      nice_debug ("Agent %p, freeing conncheck_list of stream %p", agent,
          stream);
      for (item = stream->conncheck_list; item; item = item->next)
        candidate_check_pair_free (agent, item->data);
      g_slist_free (stream->conncheck_list);
      stream->conncheck_list = NULL;
    }
  }

  conn_check_stop (agent);
}

/*
 * Prunes the list of connectivity checks for items related
 * to stream 'stream_id'. 
 *
 * @return TRUE on success, FALSE on a fatal error
 */
void conn_check_prune_stream (NiceAgent *agent, NiceStream *stream)
{
  GSList *i;
  gboolean keep_going = FALSE;

  if (stream->conncheck_list) {
    GSList *item;

    nice_debug ("Agent %p, freeing conncheck_list of stream %p", agent, stream);

    for (item = stream->conncheck_list; item; item = item->next)
      candidate_check_pair_free (agent, item->data);
    g_slist_free (stream->conncheck_list);
    stream->conncheck_list = NULL;
  }

  for (i = agent->streams; i; i = i->next) {
    NiceStream *s = i->data;
    if (s->conncheck_list) {
      keep_going = TRUE;
      break;
    }
  }

  if (!keep_going)
    conn_check_stop (agent);
}

/*
 * Fills 'dest' with a username string for use in an outbound connectivity
 * checks. No more than 'dest_len' characters (including terminating
 * NULL) is ever written to the 'dest'.
 */
static
size_t priv_gen_username (NiceAgent *agent, guint component_id,
    gchar *remote, gchar *local, uint8_t *dest, guint dest_len)
{
  guint len = 0;
  gsize remote_len = strlen (remote);
  gsize local_len = strlen (local);

  if (remote_len > 0 && local_len > 0) {
    if (agent->compatibility == NICE_COMPATIBILITY_RFC5245 &&
        dest_len >= remote_len + local_len + 1) {
      memcpy (dest, remote, remote_len);
      len += remote_len;
      memcpy (dest + len, ":", 1);
      len++;
      memcpy (dest + len, local, local_len);
      len += local_len;
    } else if ((agent->compatibility == NICE_COMPATIBILITY_WLM2009 ||
        agent->compatibility == NICE_COMPATIBILITY_OC2007R2) &&
        dest_len >= remote_len + local_len + 4 ) {
      memcpy (dest, remote, remote_len);
      len += remote_len;
      memcpy (dest + len, ":", 1);
      len++;
      memcpy (dest + len, local, local_len);
      len += local_len;
      if (len % 4 != 0) {
        memset (dest + len, 0, 4 - (len % 4));
        len += 4 - (len % 4);
      }
    } else if (agent->compatibility == NICE_COMPATIBILITY_GOOGLE &&
        dest_len >= remote_len + local_len) {
      memcpy (dest, remote, remote_len);
      len += remote_len;
      memcpy (dest + len, local, local_len);
      len += local_len;
    } else if (agent->compatibility == NICE_COMPATIBILITY_MSN ||
	       agent->compatibility == NICE_COMPATIBILITY_OC2007) {
      gchar component_str[10];
      guchar *local_decoded = NULL;
      guchar *remote_decoded = NULL;
      gsize local_decoded_len;
      gsize remote_decoded_len;
      gsize total_len;
      int padding;

      g_snprintf (component_str, sizeof(component_str), "%d", component_id);
      local_decoded = g_base64_decode (local, &local_decoded_len);
      remote_decoded = g_base64_decode (remote, &remote_decoded_len);

      total_len = remote_decoded_len + local_decoded_len + 3 + 2*strlen (component_str);
      padding = 4 - (total_len % 4);

      if (dest_len >= total_len + padding) {
        guchar pad_char[1] = {0};
        int i;

        memcpy (dest, remote_decoded, remote_decoded_len);
        len += remote_decoded_len;
        memcpy (dest + len, ":", 1);
        len++;
        memcpy (dest + len, component_str, strlen (component_str));
        len += strlen (component_str);

        memcpy (dest + len, ":", 1);
        len++;

        memcpy (dest + len, local_decoded, local_decoded_len);
        len += local_decoded_len;
        memcpy (dest + len, ":", 1);
        len++;
        memcpy (dest + len, component_str, strlen (component_str));;
        len += strlen (component_str);

        for (i = 0; i < padding; i++) {
          memcpy (dest + len, pad_char, 1);
          len++;
        }

      }

      g_free (local_decoded);
      g_free (remote_decoded);
    }
  }

  return len;
}

/*
 * Fills 'dest' with a username string for use in an outbound connectivity
 * checks. No more than 'dest_len' characters (including terminating
 * NULL) is ever written to the 'dest'.
 */
static
size_t priv_create_username (NiceAgent *agent, NiceStream *stream,
    guint component_id, NiceCandidate *remote, NiceCandidate *local,
    uint8_t *dest, guint dest_len, gboolean inbound)
{
  gchar *local_username = NULL;
  gchar *remote_username = NULL;


  if (remote && remote->username) {
    remote_username = remote->username;
  }

  if (local && local->username) {
    local_username = local->username;
  }

  if (stream) {
    if (remote_username == NULL) {
      remote_username = stream->remote_ufrag;
    }
    if (local_username == NULL) {
      local_username = stream->local_ufrag;
    }
  }

  if (local_username && remote_username) {
    if (inbound) {
      return priv_gen_username (agent, component_id,
          local_username, remote_username, dest, dest_len);
    } else {
      return priv_gen_username (agent, component_id,
          remote_username, local_username, dest, dest_len);
    }
  }

  return 0;
}

/*
 * Returns a password string for use in an outbound connectivity
 * check.
 */
static
size_t priv_get_password (NiceAgent *agent, NiceStream *stream,
    NiceCandidate *remote, uint8_t **password)
{
  if (agent->compatibility == NICE_COMPATIBILITY_GOOGLE)
    return 0;

  if (remote && remote->password) {
    *password = (uint8_t *)remote->password;
    return strlen (remote->password);
  }

  if (stream) {
    *password = (uint8_t *)stream->remote_password;
    return strlen (stream->remote_password);
  }

  return 0;
}

/* Implement the computation specific in RFC 8445 section 14 */

static unsigned int priv_compute_conncheck_timer (NiceAgent *agent, NiceStream *stream)
{
  GSList *i, *j;
  guint waiting_and_in_progress = 0;
  unsigned int rto = 0;

  /* we can compute precisely the number of pairs in-progress or
   * waiting for all streams, instead of limiting the value to one
   * stream, and multiplying it by the number of active streams.
   * Since RFC8445, this number of waiting and in-progress pairs
   * if maxed by the number of different foundations in the conncheck
   * list.
   */
  for (i = agent->streams; i ; i = i->next) {
    NiceStream *s = i->data;
    for (j = s->conncheck_list; j ; j = j->next) {
      CandidateCheckPair *p = j->data;
      if (p->state == NICE_CHECK_IN_PROGRESS ||
          p->state == NICE_CHECK_WAITING)
        waiting_and_in_progress++;
    }
  }

  rto = agent->timer_ta  * waiting_and_in_progress;

  nice_debug ("Agent %p : timer set to %dms, "
    "waiting+in_progress=%d", agent, MAX (rto, STUN_TIMER_DEFAULT_TIMEOUT),
    waiting_and_in_progress);
  return MAX (rto, STUN_TIMER_DEFAULT_TIMEOUT);
}

/*
 * Sends a connectivity check over candidate pair 'pair'.
 *
 * @return zero on success, non-zero on error
 */
int conn_check_send (NiceAgent *agent, CandidateCheckPair *pair)
{

  /* note: following information is supplied:
   *  - username (for USERNAME attribute)
   *  - password (for MESSAGE-INTEGRITY)
   *  - priority (for PRIORITY)
   *  - ICE-CONTROLLED/ICE-CONTROLLING (for role conflicts)
   *  - USE-CANDIDATE (if sent by the controlling agent)
   */

  uint8_t uname[NICE_STREAM_MAX_UNAME];
  NiceStream *stream;
  NiceComponent *component;
  gsize uname_len;
  uint8_t *password = NULL;
  gsize password_len;
  bool controlling = agent->controlling_mode;
 /* XXX: add API to support different nomination modes: */
  bool cand_use = controlling;
  size_t buffer_len;
  unsigned int timeout;
  StunTransaction *stun;

  if (!agent_find_component (agent, pair->stream_id, pair->component_id,
          &stream, &component))
    return -1;

  uname_len = priv_create_username (agent, stream, pair->component_id,
      pair->remote, pair->local, uname, sizeof (uname), FALSE);
  password_len = priv_get_password (agent, stream, pair->remote, &password);

  if (password != NULL &&
      (agent->compatibility == NICE_COMPATIBILITY_MSN ||
       agent->compatibility == NICE_COMPATIBILITY_OC2007)) {
    password = g_base64_decode ((gchar *) password, &password_len);
  }

  if (nice_debug_is_enabled ()) {
    gchar tmpbuf1[INET6_ADDRSTRLEN];
    gchar tmpbuf2[INET6_ADDRSTRLEN];
    nice_address_to_string (&pair->local->addr, tmpbuf1);
    nice_address_to_string (&pair->remote->addr, tmpbuf2);
    nice_debug ("Agent %p : STUN-CC REQ [%s]:%u --> [%s]:%u, socket=%u, "
        "pair=%p (c-id:%u), tie=%llu, username='%.*s' (%" G_GSIZE_FORMAT "), "
        "password='%.*s' (%" G_GSIZE_FORMAT "), prio=%08x, %s.", agent,
	     tmpbuf1, nice_address_get_port (&pair->local->addr),
	     tmpbuf2, nice_address_get_port (&pair->remote->addr),
             pair->sockptr->fileno ? g_socket_get_fd(pair->sockptr->fileno) : -1,
	     pair, pair->component_id,
	     (unsigned long long)agent->tie_breaker,
        (int) uname_len, uname, uname_len,
        (int) password_len, password, password_len,
        pair->stun_priority,
        controlling ? "controlling" : "controlled");
  }

  if (NICE_AGENT_IS_COMPATIBLE_WITH_RFC5245_OR_OC2007R2 (agent)) {
    switch (agent->nomination_mode) {
      case NICE_NOMINATION_MODE_REGULAR:
        /* We are doing regular nomination, so we set the use-candidate
         * attrib, when the controlling agent decided which valid pair to
         * resend with this flag in priv_conn_check_tick_stream()
         */
        cand_use = pair->use_candidate_on_next_check;
        nice_debug ("Agent %p : %s: set cand_use=%d "
            "(regular nomination).", agent, G_STRFUNC, cand_use);
        break;
      case NICE_NOMINATION_MODE_AGGRESSIVE:
        /* We are doing aggressive nomination, we set the use-candidate
         * attrib in every check we send, when we are the controlling
         * agent, RFC 5245, 8.1.1.2
         */
        cand_use = controlling;
        nice_debug ("Agent %p : %s: set cand_use=%d "
            "(aggressive nomination).", agent, G_STRFUNC, cand_use);
        break;
      default:
        /* Nothing to do. */
        break;
    }
  } else if (cand_use)
    pair->nominated = controlling;

  if (uname_len == 0) {
    nice_debug ("Agent %p: no credentials found, cancelling conncheck", agent);
    return -1;
  }

  stun = priv_add_stun_transaction (pair);

  buffer_len = stun_usage_ice_conncheck_create (&component->stun_agent,
      &stun->message, stun->buffer, sizeof(stun->buffer),
      uname, uname_len, password, password_len,
      cand_use, controlling, pair->stun_priority,
      agent->tie_breaker,
      pair->local->foundation,
      agent_to_ice_compatibility (agent));

  nice_debug ("Agent %p: conncheck created %zd - %p", agent, buffer_len,
      stun->message.buffer);

  if (agent->compatibility == NICE_COMPATIBILITY_MSN ||
      agent->compatibility == NICE_COMPATIBILITY_OC2007) {
    g_free (password);
  }

  if (buffer_len == 0) {
    nice_debug ("Agent %p: buffer is empty, cancelling conncheck", agent);
    priv_remove_stun_transaction (pair, stun, component);
    return -1;
  }

  if (nice_socket_is_reliable(pair->sockptr)) {
    timeout = agent->stun_reliable_timeout;
    stun_timer_start_reliable(&stun->timer, timeout);
  } else {
    timeout = priv_compute_conncheck_timer (agent, stream);
    stun_timer_start (&stun->timer, timeout, agent->stun_max_retransmissions);
  }

  stun->next_tick = g_get_monotonic_time () + timeout * 1000;

  /* TCP-ACTIVE candidate must create a new socket before sending
   * by connecting to the peer. The new socket is stored in the candidate
   * check pair, until we discover a new local peer reflexive */
  if (pair->sockptr->fileno == NULL &&
      pair->sockptr->type != NICE_SOCKET_TYPE_UDP_TURN &&
      pair->local->transport == NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE) {
    NiceStream *stream2 = NULL;
    NiceComponent *component2 = NULL;
    NiceSocket *new_socket;

    if (agent_find_component (agent, pair->stream_id, pair->component_id,
            &stream2, &component2)) {
      new_socket = nice_tcp_active_socket_connect (pair->sockptr,
          &pair->remote->addr);
      if (new_socket) {
        nice_debug ("Agent %p: add to tcp-act socket %p a new "
            "tcp connect socket %p on pair %p in s/c %d/%d",
            agent, pair->sockptr, new_socket, pair, stream->id, component->id);
        pair->sockptr = new_socket;
        _priv_set_socket_tos (agent, pair->sockptr, stream2->tos);

        nice_socket_set_writable_callback (pair->sockptr, _tcp_sock_is_writable,
            component2);

        nice_component_attach_socket (component2, new_socket);
      }
    }
  }
  /* send the conncheck */
  agent_socket_send (pair->sockptr, &pair->remote->addr,
      buffer_len, (gchar *)stun->buffer);

  if (agent->compatibility == NICE_COMPATIBILITY_OC2007R2)
    ms_ice2_legacy_conncheck_send (&stun->message, pair->sockptr,
        &pair->remote->addr);

  return 0;
}

/*
 * Implemented the pruning steps described in ICE sect 8.1.2
 * "Updating States" (ID-19) after a pair has been nominated.
 *
 * @see conn_check_update_check_list_state_failed_components()
 */
static guint priv_prune_pending_checks (NiceAgent *agent, NiceStream *stream, NiceComponent *component)
{
  GSList *i;
  guint64 priority;
  guint in_progress = 0;
  guint triggered_check = 0;
  gchar prio[NICE_CANDIDATE_PAIR_PRIORITY_MAX_SIZE];

  nice_debug ("Agent %p: Pruning pending checks for s%d/c%d",
      agent, stream->id, component->id);

  /* Called when we have at least one selected pair */
  priority = component->selected_pair.priority;
  g_assert (priority > 0);

  nice_candidate_pair_priority_to_string (priority, prio);
  nice_debug ("Agent %p : selected pair priority is %s", agent, prio);

  i = stream->conncheck_list;
  while (i) {
    CandidateCheckPair *p = i->data;
    GSList *next = i->next;

    if (p->component_id != component->id) {
      i = next;
      continue;
    }

    /* We do not remove a pair from the conncheck list if it is also in
     * the triggered check queue.  This is not what suggests the ICE
     * spec, but it proved to be more robust in the aggressive
     * nomination scenario, precisely because these pairs may have the
     * use-candidate flag set, and the peer agent may already have
     * selected such one.
     */
    if (g_slist_find (agent->triggered_check_queue, p) &&
        p->state != NICE_CHECK_IN_PROGRESS) {
      if (p->priority < priority) {
        nice_debug ("Agent %p : pair %p removed.", agent, p);
        candidate_check_pair_free (agent, p);
        stream->conncheck_list = g_slist_delete_link(stream->conncheck_list, i);
      } else
        triggered_check++;
    }

    /* step: cancel all FROZEN and WAITING pairs for the component */
    else if (p->state == NICE_CHECK_FROZEN || p->state == NICE_CHECK_WAITING) {
      nice_debug ("Agent %p : pair %p removed.", agent, p);
      candidate_check_pair_free (agent, p);
      stream->conncheck_list = g_slist_delete_link(stream->conncheck_list, i);
    }

    /* note: a SHOULD level req. in ICE 8.1.2. "Updating States" (ID-19) */
    else if (p->state == NICE_CHECK_IN_PROGRESS) {
      if (p->priority < priority) {
        priv_remove_pair_from_triggered_check_queue (agent, p);
        if (p->retransmit) {
          p->retransmit = FALSE;
          nice_debug ("Agent %p : pair %p will not be retransmitted.",
              agent, p);
        }
      } else {
        /* We must keep the higher priority pairs running because if a udp
         * packet was lost, we might end up using a bad candidate */
        nice_candidate_pair_priority_to_string (p->priority, prio);
        nice_debug ("Agent %p : pair %p kept IN_PROGRESS because priority "
            "%s is higher than priority of best nominated pair.", agent, p, prio);
        /* We may also have to enable the retransmit flag of pairs with
         * a higher priority than the first nominated pair
         */
        if (!p->retransmit && p->stun_transactions) {
          p->retransmit = TRUE;
          nice_debug ("Agent %p : pair %p will be retransmitted.", agent, p);
        }
        in_progress++;
      }
    }
    i = next;
  }

  return in_progress + triggered_check;
}

/*
 * Schedules a triggered check after a successfully inbound 
 * connectivity check. Implements ICE sect 7.2.1.4 "Triggered Checks" (ID-19).
 * 
 * @param agent self pointer
 * @param component the check is related to
 * @param local_socket socket from which the inbound check was received
 * @param remote_cand remote candidate from which the inbound check was sent
 */
static gboolean priv_schedule_triggered_check (NiceAgent *agent, NiceStream *stream, NiceComponent *component, NiceSocket *local_socket, NiceCandidate *remote_cand)
{
  GSList *i;
  NiceCandidate *local = NULL;
  CandidateCheckPair *p;

  g_assert (remote_cand != NULL);

  nice_debug ("Agent %p : scheduling triggered check with socket=%p "
      "and remote cand=%p.", agent, local_socket, remote_cand);

  for (i = stream->conncheck_list; i ; i = i->next) {
      p = i->data;
      if (p->component_id == component->id &&
	  p->remote == remote_cand &&
          p->sockptr == local_socket) {
        /* If we match with a peer-reflexive discovered pair, we
         * use the parent succeeded pair instead */

        if (p->succeeded_pair != NULL) {
          g_assert_cmpint (p->state, ==, NICE_CHECK_DISCOVERED);
          p = p->succeeded_pair;
        }

	nice_debug ("Agent %p : Found a matching pair %p (%s) (%s) ...",
            agent, p, p->foundation, priv_state_to_string (p->state));
	
	switch (p->state) {
          case NICE_CHECK_WAITING:
	  case NICE_CHECK_FROZEN:
            nice_debug ("Agent %p : pair %p added for a triggered check.",
                agent, p);
            priv_add_pair_to_triggered_check_queue (agent, p);
            break;
          case NICE_CHECK_IN_PROGRESS:
            /* note: according to ICE SPEC sect 7.2.1.4 "Triggered Checks"
             * we cancel the in-progress transaction, and after the
             * retransmission timeout, we create a new connectivity check
             * for that pair.  The controlling role of this new check may
             * be different from the role of this cancelled check.
             *
             * When another pair, with a higher priority is already
             * nominated, so there's no reason to recheck this pair,
             * since it can in no way replace the nominated one.
             */
            if (p->priority > component->selected_pair.priority) {
              nice_debug ("Agent %p : pair %p added for a triggered check.",
                  agent, p);
              priv_add_pair_to_triggered_check_queue (agent, p);
            }
            break;
          case NICE_CHECK_FAILED:
            if (p->priority > component->selected_pair.priority) {
                nice_debug ("Agent %p : pair %p added for a triggered check.",
                    agent, p);
                priv_add_pair_to_triggered_check_queue (agent, p);
                /* If the component for this pair is in failed state, move it
                 * back to connecting, and reinitiate the timers
                 */
                if (component->state == NICE_COMPONENT_STATE_FAILED) {
                  agent_signal_component_state_change (agent, stream->id,
                      component->id, NICE_COMPONENT_STATE_CONNECTING);
                  conn_check_schedule_next (agent);
                /* If the component if in ready state, move it back to
                 * connected as this failed pair with a higher priority
                 * than the nominated pair requires to pursue the
                 * conncheck
                 */
                } else if (component->state == NICE_COMPONENT_STATE_READY) {
                  agent_signal_component_state_change (agent, stream->id,
                      component->id, NICE_COMPONENT_STATE_CONNECTED);
                  conn_check_schedule_next (agent);
                }
            }
            break;
	  case NICE_CHECK_SUCCEEDED:
            nice_debug ("Agent %p : nothing to do for pair %p.", agent, p);
            break;
          default:
            break;
        }

	/* note: the spec says the we SHOULD retransmit in-progress
	 *       checks immediately, but we won't do that now */

	return TRUE;
      }
  }

  for (i = component->local_candidates; i ; i = i->next) {
    local = i->data;
    if (local->sockptr == local_socket)
      break;
  }

  if (i) {
    nice_debug ("Agent %p : Adding a triggered check to conn.check list (local=%p).", agent, local);
    p = priv_conn_check_add_for_candidate_pair_matched (agent, stream->id,
        component, local, remote_cand, NICE_CHECK_WAITING);
    if (p)
      priv_add_pair_to_triggered_check_queue (agent, p);
    return TRUE;
  }
  else {
    nice_debug ("Agent %p : Didn't find a matching pair for triggered check (remote-cand=%p).", agent, remote_cand);
    return FALSE;
  }
}


/*
 * Sends a reply to an successfully received STUN connectivity 
 * check request. Implements parts of the ICE spec section 7.2 (STUN
 * Server Procedures).
 *
 * @param agent context pointer
 * @param stream which stream (of the agent)
 * @param component which component (of the stream)
 * @param rcand remote candidate from which the request came, if NULL,
 *        the response is sent immediately but no other processing is done
 * @param toaddr address to which reply is sent
 * @param socket the socket over which the request came
 * @param rbuf_len length of STUN message to send
 * @param msg the STUN message to send
 * @param use_candidate whether the request had USE_CANDIDATE attribute
 * 
 * @pre (rcand == NULL || nice_address_equal(rcand->addr, toaddr) == TRUE)
 */
static void priv_reply_to_conn_check (NiceAgent *agent, NiceStream *stream,
    NiceComponent *component, NiceCandidate *lcand, NiceCandidate *rcand,
    const NiceAddress *toaddr, NiceSocket *sockptr, size_t rbuf_len,
    StunMessage *msg, gboolean use_candidate)
{
  g_assert (rcand == NULL || nice_address_equal(&rcand->addr, toaddr) == TRUE);

  if (nice_debug_is_enabled ()) {
    gchar tmpbuf[INET6_ADDRSTRLEN];
    nice_address_to_string (toaddr, tmpbuf);
    nice_debug ("Agent %p : STUN-CC RESP to '%s:%u', socket=%u, len=%u, cand=%p (c-id:%u), use-cand=%d.", agent,
	     tmpbuf,
	     nice_address_get_port (toaddr),
             sockptr->fileno ? g_socket_get_fd(sockptr->fileno) : -1,
	     (unsigned)rbuf_len,
	     rcand, component->id,
	     (int)use_candidate);
  }

  agent_socket_send (sockptr, toaddr, rbuf_len, (const gchar*)msg->buffer);
  if (agent->compatibility == NICE_COMPATIBILITY_OC2007R2) {
    ms_ice2_legacy_conncheck_send(msg, sockptr, toaddr);
  }

  /* We react to this stun request when we have the remote credentials.
   * When credentials are not yet known, this request is stored
   * in incoming_checks for later processing when returning from this
   * function.
   */
  if (rcand && stream->remote_ufrag[0]) {
    priv_schedule_triggered_check (agent, stream, component, sockptr, rcand);
    if (use_candidate)
      priv_mark_pair_nominated (agent, stream, component, lcand, rcand);
  }
}

/*
 * Stores information of an incoming STUN connectivity check
 * for later use. This is only needed when a check is received
 * before we get information about the remote candidates (via
 * SDP or other signaling means).
 *
 * @return non-zero on error, zero on success
 */
static int priv_store_pending_check (NiceAgent *agent, NiceComponent *component,
    const NiceAddress *from, NiceSocket *sockptr, uint8_t *username,
    uint16_t username_len, uint32_t priority, gboolean use_candidate)
{
  IncomingCheck *icheck;
  nice_debug ("Agent %p : Storing pending check.", agent);

  if (g_queue_get_length (&component->incoming_checks) >=
      NICE_AGENT_MAX_REMOTE_CANDIDATES) {
    nice_debug ("Agent %p : WARN: unable to store information for early incoming check.", agent);
    return -1;
  }

  icheck = g_slice_new0 (IncomingCheck);
  g_queue_push_tail (&component->incoming_checks, icheck);
  icheck->from = *from;
  icheck->local_socket = sockptr;
  icheck->priority = priority;
  icheck->use_candidate = use_candidate;
  icheck->username_len = username_len;
  icheck->username = NULL;
  if (username_len > 0)
    icheck->username = g_memdup (username, username_len);

  return 0;
}

/*
 * Adds a new pair, discovered from an incoming STUN response, to 
 * the connectivity check list.
 *
 * @return created pair, or NULL on fatal (memory allocation) errors
 */
static CandidateCheckPair *priv_add_peer_reflexive_pair (NiceAgent *agent, guint stream_id, NiceComponent *component, NiceCandidate *local_cand, CandidateCheckPair *parent_pair)
{
  CandidateCheckPair *pair = g_slice_new0 (CandidateCheckPair);
  NiceStream *stream = agent_find_stream (agent, stream_id);

  pair->stream_id = stream_id;
  pair->component_id = component->id;;
  pair->local = local_cand;
  pair->remote = parent_pair->remote;
  pair->sockptr = local_cand->sockptr;
  parent_pair->discovered_pair = pair;
  pair->succeeded_pair = parent_pair;
  nice_debug ("Agent %p : creating a new pair", agent);
  SET_PAIR_STATE (agent, pair, NICE_CHECK_DISCOVERED);
  {
      gchar tmpbuf1[INET6_ADDRSTRLEN];
      gchar tmpbuf2[INET6_ADDRSTRLEN];
      nice_address_to_string (&pair->local->addr, tmpbuf1);
      nice_address_to_string (&pair->remote->addr, tmpbuf2);
      nice_debug ("Agent %p : new pair %p : [%s]:%u --> [%s]:%u", agent, pair,
          tmpbuf1, nice_address_get_port (&pair->local->addr),
          tmpbuf2, nice_address_get_port (&pair->remote->addr));
  }
  g_snprintf (pair->foundation, NICE_CANDIDATE_PAIR_MAX_FOUNDATION, "%s:%s",
      local_cand->foundation, parent_pair->remote->foundation);

  if (agent->controlling_mode == TRUE)
    pair->priority = nice_candidate_pair_priority (pair->local->priority,
        pair->remote->priority);
  else
    pair->priority = nice_candidate_pair_priority (pair->remote->priority,
        pair->local->priority);
  pair->nominated = parent_pair->nominated;
  /* the peer-reflexive priority used in stun request is copied from
   * the parent succeeded pair. This value is not required for discovered
   * pair, that won't emit stun requests themselves, but may be used when
   * such pair becomes the selected pair, and when keepalive stun are emitted,
   * using the sockptr and stun_priority values from the succeeded pair.
   */
  pair->stun_priority = parent_pair->stun_priority;
  nice_debug ("Agent %p : added a new peer-discovered pair %p with "
      "foundation '%s' and transport %s:%s to stream %u component %u",
      agent, pair, pair->foundation,
      priv_candidate_transport_to_string (pair->local->transport),
      priv_candidate_transport_to_string (pair->remote->transport),
      stream_id, component->id);

  stream->conncheck_list = g_slist_insert_sorted (stream->conncheck_list, pair,
      (GCompareFunc)conn_check_compare);

  return pair;
}

/*
 * Recalculates priorities of all candidate pairs. This
 * is required after a conflict in ICE roles.
 */
void recalculate_pair_priorities (NiceAgent *agent)
{
  GSList *i, *j;

  for (i = agent->streams; i; i = i->next) {
    NiceStream *stream = i->data;
    for (j = stream->conncheck_list; j; j = j->next) {
      CandidateCheckPair *p = j->data;
      p->priority = agent_candidate_pair_priority (agent, p->local, p->remote);
    }
    stream->conncheck_list = g_slist_sort (stream->conncheck_list,
        (GCompareFunc)conn_check_compare);
  }
}

/*
 * Change the agent role if different from 'control'. Can be
 * initiated both by handling of incoming connectivity checks,
 * and by processing the responses to checks sent by us.
 */
static void priv_check_for_role_conflict (NiceAgent *agent, gboolean control)
{
  /* role conflict, change mode; wait for a new conn. check */
  if (control != agent->controlling_mode) {
    nice_debug ("Agent %p : Role conflict, changing agent role to \"%s\".",
        agent, control ? "controlling" : "controlled");
    agent->controlling_mode = control;
    /* the pair priorities depend on the roles, so recalculation
     * is needed */
    recalculate_pair_priorities (agent);
  }
  else 
    nice_debug ("Agent %p : Role conflict, staying with role \"%s\".",
        agent, control ? "controlling" : "controlled");
}

/*
 * Checks whether the mapped address in connectivity check response 
 * matches any of the known local candidates. If not, apply the
 * mechanism for "Discovering Peer Reflexive Candidates" ICE ID-19)
 *
 * @param agent context pointer
 * @param stream which stream (of the agent)
 * @param component which component (of the stream)
 * @param p the connectivity check pair for which we got a response
 * @param socketptr socket used to send the reply
 * @param mapped_sockaddr mapped address in the response
 *
 * @return pointer to a candidate pair, found in conncheck list or newly created
 */
static CandidateCheckPair *priv_process_response_check_for_reflexive(NiceAgent *agent, NiceStream *stream, NiceComponent *component, CandidateCheckPair *p, NiceSocket *sockptr, struct sockaddr *mapped_sockaddr, NiceCandidate *local_candidate, NiceCandidate *remote_candidate)
{
  CandidateCheckPair *new_pair = NULL;
  NiceAddress mapped;
  GSList *i;
  NiceCandidate *local_cand = NULL;

  nice_address_set_from_sockaddr (&mapped, mapped_sockaddr);

  for (i = component->local_candidates; i; i = i->next) {
    NiceCandidate *cand = i->data;

    if (nice_address_equal (&mapped, &cand->addr) &&
        local_candidate_and_socket_compatible (agent, cand, sockptr)) {
      local_cand = cand;
      break;
    }
  }

  /* The mapped address allows to look for a previously discovered
   * peer reflexive local candidate, and its related pair. This
   * new_pair will be marked 'Valid', while the pair 'p' of the
   * initial stun request will be marked 'Succeeded'
   *
   * In the case of a tcp-act/tcp-pass pair 'p', where the local
   * candidate is of type tcp-act, and its port number is zero, a
   * conncheck on this pair *always* leads to the creation of a
   * discovered peer-reflexive tcp-act local candidate.
   */
  for (i = stream->conncheck_list; i; i = i->next) {
    CandidateCheckPair *pair = i->data;
    if (local_cand == pair->local && remote_candidate == pair->remote) {
      new_pair = pair;
      break;
    }
  }

  if (new_pair) {
    /* note: when new_pair is distinct from p, it means new_pair is a
     * previously discovered peer-reflexive candidate pair, so we don't
     * set the valid flag on p in this case, because the valid flag is
     * already set on the discovered pair.
     */
    if (new_pair == p)
      p->valid = TRUE;
    SET_PAIR_STATE (agent, p, NICE_CHECK_SUCCEEDED);
    priv_remove_pair_from_triggered_check_queue (agent, p);
    priv_free_all_stun_transactions (p, component);
    nice_component_add_valid_candidate (agent, component, remote_candidate);
  }
  else {
    if (local_cand == NULL && !agent->force_relay) {
      /* step: find a new local candidate, see RFC 5245 7.1.3.2.1.
       * "Discovering Peer Reflexive Candidates"
       *
       * The priority equal to the value of the PRIORITY attribute
       * in the Binding request is taken from the "parent" pair p
       */
      local_cand = discovery_add_peer_reflexive_candidate (agent,
                                                           stream->id,
                                                           component->id,
                                                           p->stun_priority,
                                                          &mapped,
                                                           sockptr,
                                                           local_candidate,
                                                           remote_candidate);
      nice_debug ("Agent %p : added a new peer-reflexive local candidate %p "
          "with transport %s", agent, local_cand,
          priv_candidate_transport_to_string (local_cand->transport));
    }

    /* step: add a new discovered pair (see RFC 5245 7.1.3.2.2
	       "Constructing a Valid Pair") */
    if (local_cand)
      new_pair = priv_add_peer_reflexive_pair (agent, stream->id, component,
          local_cand, p);
    /* note: this is same as "adding to VALID LIST" in the spec
       text */
    if (new_pair)
      new_pair->valid = TRUE;
    /* step: The agent sets the state of the pair that *generated* the check to
     * Succeeded, RFC 5245, 7.1.3.2.3, "Updating Pair States"
     */
    SET_PAIR_STATE (agent, p, NICE_CHECK_SUCCEEDED);
    priv_remove_pair_from_triggered_check_queue (agent, p);
    priv_free_all_stun_transactions (p, component);
  }

  if (new_pair && new_pair->valid)
    nice_component_add_valid_candidate (agent, component, remote_candidate);


  return new_pair;
}

/*
 * Tries to match STUN reply in 'buf' to an existing STUN connectivity
 * check transaction. If found, the reply is processed. Implements
 * section 7.1.2 "Processing the Response" of ICE spec (ID-19).
 *
 * @return TRUE if a matching transaction is found
 */
static gboolean priv_map_reply_to_conn_check_request (NiceAgent *agent, NiceStream *stream, NiceComponent *component, NiceSocket *sockptr, const NiceAddress *from, NiceCandidate *local_candidate, NiceCandidate *remote_candidate, StunMessage *resp)
{
  union {
    struct sockaddr_storage storage;
    struct sockaddr addr;
  } sockaddr;
  socklen_t socklen = sizeof (sockaddr);
  GSList *i, *j;
  guint k;
  StunUsageIceReturn res;
  StunTransactionId discovery_id;
  StunTransactionId response_id;
  stun_message_id (resp, response_id);

  for (i = stream->conncheck_list; i; i = i->next) {
    CandidateCheckPair *p = i->data;

    for (j = p->stun_transactions, k = 0; j; j = j->next, k++) {
      StunTransaction *stun = j->data;

      stun_message_id (&stun->message, discovery_id);

      if (memcmp (discovery_id, response_id, sizeof(StunTransactionId)))
	continue;

      res = stun_usage_ice_conncheck_process (resp,
	  &sockaddr.storage, &socklen,
	  agent_to_ice_compatibility (agent));
      nice_debug ("Agent %p : stun_bind_process/conncheck for %p: "
	  "%s,res=%s,stun#=%d.",
	  agent, p,
	  agent->controlling_mode ? "controlling" : "controlled",
	  priv_ice_return_to_string (res), k);

      if (res == STUN_USAGE_ICE_RETURN_SUCCESS ||
	  res == STUN_USAGE_ICE_RETURN_NO_MAPPED_ADDRESS) {
	/* case: found a matching connectivity check request */

	CandidateCheckPair *ok_pair = NULL;

	nice_debug ("Agent %p : pair %p MATCHED.", agent, p);
	priv_remove_stun_transaction (p, stun, component);

	/* step: verify that response came from the same IP address we
	 *       sent the original request to (see 7.1.2.1. "Failure
	 *       Cases") */
	if (nice_address_equal (from, &p->remote->addr) == FALSE) {
	  candidate_check_pair_fail (stream, agent, p);
	  if (nice_debug_is_enabled ()) {
	    gchar tmpbuf[INET6_ADDRSTRLEN];
	    gchar tmpbuf2[INET6_ADDRSTRLEN];
	    nice_debug ("Agent %p : pair %p FAILED"
		" (mismatch of source address).", agent, p);
	    nice_address_to_string (&p->remote->addr, tmpbuf);
	    nice_address_to_string (from, tmpbuf2);
	    nice_debug ("Agent %p : '%s:%u' != '%s:%u'", agent,
		tmpbuf, nice_address_get_port (&p->remote->addr),
		tmpbuf2, nice_address_get_port (from));
	  }
	  return TRUE;
	}

        if (remote_candidate == NULL) {
          candidate_check_pair_fail (stream, agent, p);
          if (nice_debug_is_enabled ()) {
            nice_debug ("Agent %p : pair %p FAILED "
                "(got a matching pair without a known remote candidate).", agent, p);
          }
          return TRUE;
        }

	/* note: CONNECTED but not yet READY, see docs */

	/* step: handle the possible case of a peer-reflexive
	 *       candidate where the mapped-address in response does
	 *       not match any local candidate, see 7.1.2.2.1
	 *       "Discovering Peer Reflexive Candidates" ICE ID-19) */

        if (res == STUN_USAGE_ICE_RETURN_NO_MAPPED_ADDRESS) {
          nice_debug ("Agent %p : Mapped address not found", agent);
          SET_PAIR_STATE (agent, p, NICE_CHECK_SUCCEEDED);
          p->valid = TRUE;
          nice_component_add_valid_candidate (agent, component, p->remote);
        } else
          ok_pair = priv_process_response_check_for_reflexive (agent,
              stream, component, p, sockptr, &sockaddr.addr,
              local_candidate, remote_candidate);

	/* note: The success of this check might also
	 * cause the state of other checks to change as well
         * See sect 7.2.5.3.3 (Updating Candidate Pair States) of
         * ICE spec (RFC8445).
	 */
	conn_check_unfreeze_related (agent, p);

	/* Note: this assignment helps to reduce the numbers of cases
	 * to be tested. If ok_pair and p refer to distinct pairs, it
	 * means that ok_pair is a discovered peer reflexive one,
	 * caused by the check made on pair p.  In that case, the
	 * flags to be tested are on p, but the nominated flag will be
	 * set on ok_pair. When there's no discovered pair, p and
	 * ok_pair refer to the same pair.
	 * To summarize : p is a SUCCEEDED pair, ok_pair is a
	 * DISCOVERED, VALID, and eventually NOMINATED pair. 
	 */
	if (!ok_pair)
	  ok_pair = p;

	/* step: updating nominated flag (ICE 7.1.2.2.4 "Updating the
	   Nominated Flag" (ID-19) */
	if (NICE_AGENT_IS_COMPATIBLE_WITH_RFC5245_OR_OC2007R2 (agent)) {
	  nice_debug ("Agent %p : Updating nominated flag (%s): "
	      "ok_pair=%p (%d/%d) p=%p (%d/%d) (ucnc/mnora)",
	      agent, p->local->transport == NICE_CANDIDATE_TRANSPORT_UDP ?
		"UDP" : "TCP",
	      ok_pair, ok_pair->use_candidate_on_next_check,
	      ok_pair->mark_nominated_on_response_arrival,
	      p, p->use_candidate_on_next_check,
	      p->mark_nominated_on_response_arrival);

	  if (agent->controlling_mode) {
	    switch (agent->nomination_mode) {
	      case NICE_NOMINATION_MODE_REGULAR:
		if (p->use_candidate_on_next_check) {
		  nice_debug ("Agent %p : marking pair %p (%s) as nominated "
		      "(regular nomination, controlling, "
		      "use_cand_on_next_check=1).",
		      agent, ok_pair, ok_pair->foundation);
		  ok_pair->nominated = TRUE;
		}
		break;
	      case NICE_NOMINATION_MODE_AGGRESSIVE:
		if (!p->nominated) {
		  nice_debug ("Agent %p : marking pair %p (%s) as nominated "
		      "(aggressive nomination, controlling).",
		      agent, ok_pair, ok_pair->foundation);
		  ok_pair->nominated = TRUE;
		}
		break;
	      default:
		/* Nothing to do */
		break;
	    }
	  } else {
	    if (p->mark_nominated_on_response_arrival) {
	      nice_debug ("Agent %p : marking pair %p (%s) as nominated "
		  "(%s nomination, controlled, mark_on_response=1).",
		  agent, ok_pair, ok_pair->foundation,
		  agent->nomination_mode == NICE_NOMINATION_MODE_AGGRESSIVE ?
		    "aggressive" : "regular");
	      ok_pair->nominated = TRUE;
	    }
	  }
	}

	if (ok_pair->nominated == TRUE) {
          conn_check_update_selected_pair (agent, component, ok_pair);
	  priv_print_conn_check_lists (agent, G_STRFUNC,
	      ", got a nominated pair");

	  /* Do not step down to CONNECTED if we're already at state READY*/
	  if (component->state != NICE_COMPONENT_STATE_READY)
	    /* step: notify the client of a new component state (must be done
	     *       before the possible check list state update step */
	    agent_signal_component_state_change (agent,
		stream->id, component->id, NICE_COMPONENT_STATE_CONNECTED);
	}

	/* step: update pair states (ICE 7.1.2.2.3 "Updating pair
	   states" and 8.1.2 "Updating States", ID-19) */
	conn_check_update_check_list_state_for_ready (agent, stream, component);
      } else if (res == STUN_USAGE_ICE_RETURN_ROLE_CONFLICT) {
        uint64_t tie;
        gboolean controlled_mode;

        if (!p->retransmit) {
          nice_debug ("Agent %p : Role conflict with pair %p, not restarting",
              agent, p);
          return TRUE;
        }

	/* case: role conflict error, need to restart with new role */
	nice_debug ("Agent %p : Role conflict with pair %p, restarting",
            agent, p);

	/* note: this res value indicates that the role of the peer
	 * agent has not changed after the tie-breaker comparison, so
	 * this is our role that must change. see ICE sect. 7.1.3.1
	 * "Failure Cases". Our role might already have changed due to
	 * an earlier incoming request, but if not, change role now.
	 *
	 * Sect. 7.1.3.1 is not clear on this point, but we choose to
	 * put the candidate pair in the triggered check list even
	 * when the agent did not switch its role. The reason for this
	 * interpretation is that the reception of the stun reply, even
	 * an error reply, is a good sign that this pair will be
	 * valid, if we retry the check after the role of both peers
	 * has been fixed.
	 */
        controlled_mode = (stun_message_find64 (&stun->message,
            STUN_ATTRIBUTE_ICE_CONTROLLED, &tie) ==
            STUN_MESSAGE_RETURN_SUCCESS);

        priv_check_for_role_conflict (agent, controlled_mode);
	priv_remove_stun_transaction (p, stun, component);
        priv_add_pair_to_triggered_check_queue (agent, p);
      } else {
	/* case: STUN error, the check STUN context was freed */
	candidate_check_pair_fail (stream, agent, p);
      }
      return TRUE;
    }
  }

  return FALSE;
}

/*
 * Tries to match STUN reply in 'buf' to an existing STUN discovery
 * transaction. If found, a reply is sent.
 *
 * @return TRUE if a matching transaction is found
 */
static gboolean priv_map_reply_to_discovery_request (NiceAgent *agent, StunMessage *resp)
{
  union {
    struct sockaddr_storage storage;
    struct sockaddr addr;
  } sockaddr;
  socklen_t socklen = sizeof (sockaddr);

  union {
    struct sockaddr_storage storage;
    struct sockaddr addr;
  } alternate;
  socklen_t alternatelen = sizeof (sockaddr);

  GSList *i;
  StunUsageBindReturn res;
  gboolean trans_found = FALSE;
  StunTransactionId discovery_id;
  StunTransactionId response_id;
  stun_message_id (resp, response_id);

  for (i = agent->discovery_list; i && trans_found != TRUE; i = i->next) {
    CandidateDiscovery *d = i->data;

    if (d->type == NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE &&
        d->stun_message.buffer) {
      stun_message_id (&d->stun_message, discovery_id);

      if (memcmp (discovery_id, response_id, sizeof(StunTransactionId)) == 0) {
        res = stun_usage_bind_process (resp, &sockaddr.addr,
            &socklen, &alternate.addr, &alternatelen);
        nice_debug ("Agent %p : stun_bind_process/disc for %p res %d.",
            agent, d, (int)res);

        if (res == STUN_USAGE_BIND_RETURN_ALTERNATE_SERVER) {
          /* handle alternate server */
          NiceAddress niceaddr;
          nice_address_set_from_sockaddr (&niceaddr, &alternate.addr);
          d->server = niceaddr;

          d->pending = FALSE;
          agent->discovery_unsched_items++;
        } else if (res == STUN_USAGE_BIND_RETURN_SUCCESS) {
          /* case: successful binding discovery, create a new local candidate */

          if (!agent->force_relay) {
            NiceAddress niceaddr;

            nice_address_set_from_sockaddr (&niceaddr, &sockaddr.addr);
            discovery_add_server_reflexive_candidate (
                agent,
                d->stream_id,
                d->component_id,
                &niceaddr,
                NICE_CANDIDATE_TRANSPORT_UDP,
                d->nicesock,
                FALSE);
            if (agent->use_ice_tcp)
              discovery_discover_tcp_server_reflexive_candidates (
                  agent,
                  d->stream_id,
                  d->component_id,
                  &niceaddr,
                  d->nicesock);
          }
          d->stun_message.buffer = NULL;
          d->stun_message.buffer_len = 0;
          d->done = TRUE;
          trans_found = TRUE;
        } else if (res == STUN_USAGE_BIND_RETURN_ERROR) {
          /* case: STUN error, the check STUN context was freed */
          d->stun_message.buffer = NULL;
          d->stun_message.buffer_len = 0;
          d->done = TRUE;
          trans_found = TRUE;
        }
      }
    }
  }

  return trans_found;
}

static guint
priv_calc_turn_timeout (guint lifetime)
{
  if (lifetime > 120)
    return lifetime - 60;
  else
    return lifetime / 2;
}

static void
priv_add_new_turn_refresh (NiceAgent *agent, CandidateDiscovery *cdisco,
    NiceCandidate *relay_cand, guint lifetime)
{
  CandidateRefresh *cand;

  if (cdisco->turn->type == NICE_RELAY_TYPE_TURN_TLS &&
      (agent->compatibility == NICE_COMPATIBILITY_OC2007 ||
       agent->compatibility == NICE_COMPATIBILITY_OC2007R2))
    return;

  cand = g_slice_new0 (CandidateRefresh);
  agent->refresh_list = g_slist_append (agent->refresh_list, cand);

  cand->candidate = relay_cand;
  cand->nicesock = cdisco->nicesock;
  cand->server = cdisco->server;
  cand->stream_id = cdisco->stream_id;
  cand->component_id = cdisco->component_id;
  memcpy (&cand->stun_agent, &cdisco->stun_agent, sizeof(StunAgent));

  /* Use previous stun response for authentication credentials */
  if (cdisco->stun_resp_msg.buffer != NULL) {
    memcpy(cand->stun_resp_buffer, cdisco->stun_resp_buffer,
        sizeof(cand->stun_resp_buffer));
    memcpy(&cand->stun_resp_msg, &cdisco->stun_resp_msg, sizeof(StunMessage));
    cand->stun_resp_msg.buffer = cand->stun_resp_buffer;
    cand->stun_resp_msg.agent = &cand->stun_agent;
    cand->stun_resp_msg.key = NULL;
  }

  nice_debug ("Agent %p : Adding new refresh candidate %p with timeout %d",
      agent, cand, priv_calc_turn_timeout (lifetime));
  /* step: also start the refresh timer */
  /* refresh should be sent 1 minute before it expires */
  agent_timeout_add_seconds_with_context (agent, &cand->timer_source,
      "Candidate TURN refresh",
      priv_calc_turn_timeout (lifetime),
      priv_turn_allocate_refresh_tick_agent_locked, cand);

  nice_debug ("timer source is : %p", cand->timer_source);

  return;
}

static void priv_handle_turn_alternate_server (NiceAgent *agent,
    CandidateDiscovery *disco, NiceAddress server, NiceAddress alternate)
{
  /* We need to cancel and reset all candidate discovery turn for the same
     stream and type if there is an alternate server. Otherwise, we might end up
     with two relay components on different servers, creating candidates with
     unique foundations that only contain one component.
  */
  GSList *i;

  for (i = agent->discovery_list; i; i = i->next) {
    CandidateDiscovery *d = i->data;

    if (!d->done &&
        d->type == disco->type &&
        d->stream_id == disco->stream_id &&
        d->turn->type == disco->turn->type &&
        nice_address_equal (&d->server, &server)) {
      gchar ip[INET6_ADDRSTRLEN];
      // Cancel the pending request to avoid a race condition with another
      // component responding with another altenrate-server
      d->stun_message.buffer = NULL;
      d->stun_message.buffer_len = 0;

      nice_address_to_string (&server, ip);
      nice_debug ("Agent %p : Cancelling and setting alternate server %s for "
          "CandidateDiscovery %p on s%d/c%d", agent, ip, d,
          d->stream_id, d->component_id);
      d->server = alternate;
      d->turn->server = alternate;
      d->pending = FALSE;
      agent->discovery_unsched_items++;

      if (d->turn->type == NICE_RELAY_TYPE_TURN_TCP ||
          d->turn->type == NICE_RELAY_TYPE_TURN_TLS) {
        NiceStream *stream;
        NiceComponent *component;

        if (!agent_find_component (agent, d->stream_id, d->component_id,
            &stream, &component)) {
          nice_debug ("Could not find stream or component in "
              "priv_handle_turn_alternate_server");
          continue;
        }
        d->nicesock = agent_create_tcp_turn_socket (agent, stream, component,
            d->nicesock, &d->server, d->turn->type,
            nice_socket_is_reliable (d->nicesock));

        nice_component_attach_socket (component, d->nicesock);
      }
    }
  }
}

/*
 * Tries to match STUN reply in 'buf' to an existing STUN discovery
 * transaction. If found, a reply is sent.
 * 
 * @return TRUE if a matching transaction is found
 */
static gboolean priv_map_reply_to_relay_request (NiceAgent *agent, StunMessage *resp)
{
  union {
    struct sockaddr_storage storage;
    struct sockaddr addr;
  } sockaddr;
  socklen_t socklen = sizeof (sockaddr);

  union {
    struct sockaddr_storage storage;
    struct sockaddr addr;
  } alternate;
  socklen_t alternatelen = sizeof (alternate);

  union {
    struct sockaddr_storage storage;
    struct sockaddr addr;
  } relayaddr;
  socklen_t relayaddrlen = sizeof (relayaddr);

  uint32_t lifetime;
  uint32_t bandwidth;
  GSList *i;
  StunUsageTurnReturn res;
  gboolean trans_found = FALSE;
  StunTransactionId discovery_id;
  StunTransactionId response_id;
  stun_message_id (resp, response_id);

  for (i = agent->discovery_list; i && trans_found != TRUE; i = i->next) {
    CandidateDiscovery *d = i->data;

    if (d->type == NICE_CANDIDATE_TYPE_RELAYED &&
        d->stun_message.buffer) {
      stun_message_id (&d->stun_message, discovery_id);

      if (memcmp (discovery_id, response_id, sizeof(StunTransactionId)) == 0) {
        res = stun_usage_turn_process (resp,
            &relayaddr.storage, &relayaddrlen,
            &sockaddr.storage, &socklen,
            &alternate.storage, &alternatelen,
            &bandwidth, &lifetime, agent_to_turn_compatibility (agent));
        nice_debug ("Agent %p : stun_turn_process/disc for %p res %d.",
            agent, d, (int)res);

        if (res == STUN_USAGE_TURN_RETURN_ALTERNATE_SERVER) {
          NiceAddress addr;

          /* handle alternate server */
          nice_address_set_from_sockaddr (&addr, &alternate.addr);
          priv_handle_turn_alternate_server (agent, d, d->server, addr);
          trans_found = TRUE;
        } else if (res == STUN_USAGE_TURN_RETURN_RELAY_SUCCESS ||
                   res == STUN_USAGE_TURN_RETURN_MAPPED_SUCCESS) {
          /* case: successful allocate, create a new local candidate */
          NiceAddress niceaddr;
          NiceCandidate *relay_cand;

          nice_address_set_from_sockaddr (&niceaddr, &relayaddr.addr);

          if (res == STUN_USAGE_TURN_RETURN_MAPPED_SUCCESS) {
            NiceAddress mappedniceaddr;

            /* We also received our mapped address */
            nice_address_set_from_sockaddr (&mappedniceaddr, &sockaddr.addr);

            /* TCP or TLS TURNS means the server-reflexive address was
             * on a TCP connection, which cannot be used for server-reflexive
             * discovery of candidates.
             */
            if (d->turn->type == NICE_RELAY_TYPE_TURN_UDP &&
                !agent->force_relay) {
              discovery_add_server_reflexive_candidate (
                  agent,
                  d->stream_id,
                  d->component_id,
                  &mappedniceaddr,
                  NICE_CANDIDATE_TRANSPORT_UDP,
                  d->nicesock,
                  FALSE);
            }
            if (agent->use_ice_tcp) {
              if ((agent->compatibility == NICE_COMPATIBILITY_OC2007 ||
                   agent->compatibility == NICE_COMPATIBILITY_OC2007R2) &&
                  !nice_address_equal_no_port (&niceaddr, &d->turn->server)) {
                  nice_debug("TURN port got allocated on an alternate server, "
                             "ignoring bogus srflx address");
              } else {
                discovery_discover_tcp_server_reflexive_candidates (
                    agent,
                    d->stream_id,
                    d->component_id,
                    &mappedniceaddr,
                    d->nicesock);
              }
            }
          }

          if (nice_socket_is_reliable (d->nicesock)) {
            relay_cand = discovery_add_relay_candidate (
                agent,
                d->stream_id,
                d->component_id,
                &niceaddr,
                NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE,
                d->nicesock,
                d->turn);

            if (relay_cand) {
              if (agent->compatibility == NICE_COMPATIBILITY_OC2007 ||
                  agent->compatibility == NICE_COMPATIBILITY_OC2007R2) {
                nice_udp_turn_socket_set_ms_realm(relay_cand->sockptr,
                    &d->stun_message);
                nice_udp_turn_socket_set_ms_connection_id(relay_cand->sockptr,
                    resp);
              }
              priv_add_new_turn_refresh (agent, d, relay_cand, lifetime);
            }

            relay_cand = discovery_add_relay_candidate (
                agent,
                d->stream_id,
                d->component_id,
                &niceaddr,
                NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE,
                d->nicesock,
                d->turn);
          } else {
            relay_cand = discovery_add_relay_candidate (
                agent,
                d->stream_id,
                d->component_id,
                &niceaddr,
                NICE_CANDIDATE_TRANSPORT_UDP,
                d->nicesock,
                d->turn);
          }

          if (relay_cand) {
	    if (d->stun_resp_msg.buffer)
	      nice_udp_turn_socket_cache_realm_nonce (relay_cand->sockptr,
                  &d->stun_resp_msg);
            if (agent->compatibility == NICE_COMPATIBILITY_OC2007 ||
                agent->compatibility == NICE_COMPATIBILITY_OC2007R2) {
              /* These data are needed on TURN socket when sending requests,
               * but never reach nice_turn_socket_parse_recv() where it could
               * be read directly, as the socket does not exist when allocate
               * response arrives to _nice_agent_recv(). We must set them right
               * after socket gets created in discovery_add_relay_candidate(),
               * so we are doing it here. */
              nice_udp_turn_socket_set_ms_realm(relay_cand->sockptr,
                  &d->stun_message);
              nice_udp_turn_socket_set_ms_connection_id(relay_cand->sockptr,
                  resp);
            }
            priv_add_new_turn_refresh (agent, d, relay_cand, lifetime);

            /* In case a new candidate has been added */
            conn_check_schedule_next (agent);
          }

          d->stun_message.buffer = NULL;
          d->stun_message.buffer_len = 0;
          d->done = TRUE;
          trans_found = TRUE;
        } else if (res == STUN_USAGE_TURN_RETURN_ERROR) {
          int code = -1;
          uint8_t *sent_realm = NULL;
          uint8_t *recv_realm = NULL;
          uint16_t sent_realm_len = 0;
          uint16_t recv_realm_len = 0;

          sent_realm = (uint8_t *) stun_message_find (&d->stun_message,
              STUN_ATTRIBUTE_REALM, &sent_realm_len);
          recv_realm = (uint8_t *) stun_message_find (resp,
              STUN_ATTRIBUTE_REALM, &recv_realm_len);

          if ((agent->compatibility == NICE_COMPATIBILITY_OC2007  ||
              agent->compatibility == NICE_COMPATIBILITY_OC2007R2) &&
              alternatelen != sizeof(alternate)) {
            NiceAddress addr;

            nice_address_set_from_sockaddr (&addr, &alternate.addr);

            if (!nice_address_equal (&addr, &d->server)) {
              priv_handle_turn_alternate_server (agent, d, d->server, addr);
            }
          }
          /* check for unauthorized error response */
          if ((agent->compatibility == NICE_COMPATIBILITY_RFC5245 ||
               agent->compatibility == NICE_COMPATIBILITY_OC2007  ||
               agent->compatibility == NICE_COMPATIBILITY_OC2007R2) &&
              stun_message_get_class (resp) == STUN_ERROR &&
              stun_message_find_error (resp, &code) ==
              STUN_MESSAGE_RETURN_SUCCESS &&
              recv_realm != NULL && recv_realm_len > 0) {

            if (code == STUN_ERROR_STALE_NONCE ||
                (code == STUN_ERROR_UNAUTHORIZED &&
                    !(recv_realm_len == sent_realm_len &&
                        sent_realm != NULL &&
                        memcmp (sent_realm, recv_realm, sent_realm_len) == 0))) {
              d->stun_resp_msg = *resp;
              memcpy (d->stun_resp_buffer, resp->buffer,
                  stun_message_length (resp));
              d->stun_resp_msg.buffer = d->stun_resp_buffer;
              d->stun_resp_msg.buffer_len = sizeof(d->stun_resp_buffer);
              d->pending = FALSE;
              agent->discovery_unsched_items++;
            } else {
              /* case: a real unauthorized error */
              d->stun_message.buffer = NULL;
              d->stun_message.buffer_len = 0;
              d->done = TRUE;
            }
          } else if (d->pending) {
            /* case: STUN error, the check STUN context was freed */
            d->stun_message.buffer = NULL;
            d->stun_message.buffer_len = 0;
            d->done = TRUE;
          }
          trans_found = TRUE;
        }
      }
    }
  }

  return trans_found;
}


/*
 * Tries to match STUN reply in 'buf' to an existing STUN discovery
 * transaction. If found, a reply is sent.
 * 
 * @return TRUE if a matching transaction is found
 */
static gboolean priv_map_reply_to_relay_refresh (NiceAgent *agent, StunMessage *resp)
{
  uint32_t lifetime;
  GSList *i;
  StunUsageTurnReturn res;
  gboolean trans_found = FALSE;
  StunTransactionId refresh_id;
  StunTransactionId response_id;
  stun_message_id (resp, response_id);

  for (i = agent->refresh_list; i && trans_found != TRUE;) {
    CandidateRefresh *cand = i->data;
    GSList *next = i->next;

    if (!cand->disposing && cand->stun_message.buffer) {
      stun_message_id (&cand->stun_message, refresh_id);

      if (memcmp (refresh_id, response_id, sizeof(StunTransactionId)) == 0) {
        res = stun_usage_turn_refresh_process (resp,
            &lifetime, agent_to_turn_compatibility (agent));
        nice_debug ("Agent %p : stun_turn_refresh_process for %p res %d with lifetime %u.",
            agent, cand, (int)res, lifetime);
        if (res == STUN_USAGE_TURN_RETURN_RELAY_SUCCESS) {
          /* refresh should be sent 1 minute before it expires */
          agent_timeout_add_seconds_with_context (agent,
              &cand->timer_source,
              "Candidate TURN refresh", priv_calc_turn_timeout (lifetime),
              priv_turn_allocate_refresh_tick_agent_locked, cand);

          g_source_destroy (cand->tick_source);
          g_source_unref (cand->tick_source);
          cand->tick_source = NULL;
          trans_found = TRUE;
        } else if (res == STUN_USAGE_TURN_RETURN_ERROR) {
          int code = -1;
          uint8_t *sent_realm = NULL;
          uint8_t *recv_realm = NULL;
          uint16_t sent_realm_len = 0;
          uint16_t recv_realm_len = 0;

          sent_realm = (uint8_t *) stun_message_find (&cand->stun_message,
              STUN_ATTRIBUTE_REALM, &sent_realm_len);
          recv_realm = (uint8_t *) stun_message_find (resp,
              STUN_ATTRIBUTE_REALM, &recv_realm_len);

          /* check for unauthorized error response */
          if (agent->compatibility == NICE_COMPATIBILITY_RFC5245 &&
              stun_message_get_class (resp) == STUN_ERROR &&
              stun_message_find_error (resp, &code) ==
              STUN_MESSAGE_RETURN_SUCCESS &&
              recv_realm != NULL && recv_realm_len > 0) {

            if (code == STUN_ERROR_STALE_NONCE ||
                (code == STUN_ERROR_UNAUTHORIZED &&
                    !(recv_realm_len == sent_realm_len &&
                        sent_realm != NULL &&
                        memcmp (sent_realm, recv_realm, sent_realm_len) == 0))) {
              cand->stun_resp_msg = *resp;
              memcpy (cand->stun_resp_buffer, resp->buffer,
                  stun_message_length (resp));
              cand->stun_resp_msg.buffer = cand->stun_resp_buffer;
              cand->stun_resp_msg.buffer_len = sizeof(cand->stun_resp_buffer);
              priv_turn_allocate_refresh_tick_unlocked (agent, cand);
            } else {
              /* case: a real unauthorized error */
              refresh_free (agent, cand);
            }
          } else {
            /* case: STUN error, the check STUN context was freed */
            refresh_free (agent, cand);
          }
          trans_found = TRUE;
        }
      }
    }
    i = next;
  }

  return trans_found;
}

static gboolean priv_map_reply_to_relay_remove (NiceAgent *agent,
    StunMessage *resp)
{
  StunTransactionId response_id;
  GSList *i;

  stun_message_id (resp, response_id);

  for (i = agent->refresh_list; i; i = i->next) {
    CandidateRefresh *cand = i->data;
    StunTransactionId request_id;
    StunUsageTurnReturn res;
    uint32_t lifetime;

    if (!cand->disposing || !cand->stun_message.buffer) {
      continue;
    }

    stun_message_id (&cand->stun_message, request_id);

    if (memcmp (request_id, response_id, sizeof(StunTransactionId)) == 0) {
      res = stun_usage_turn_refresh_process (resp, &lifetime,
          agent_to_turn_compatibility (agent));

      nice_debug ("Agent %p : priv_map_reply_to_relay_remove for %p res %d "
          "with lifetime %u.", agent, cand, res, lifetime);

      if (res != STUN_USAGE_TURN_RETURN_INVALID) {
        refresh_free (agent, cand);
        return TRUE;
      }
    }
  }

  return FALSE;
}

static gboolean priv_map_reply_to_keepalive_conncheck (NiceAgent *agent,
    NiceComponent *component, StunMessage *resp)
{
  StunTransactionId conncheck_id;
  StunTransactionId response_id;
  stun_message_id (resp, response_id);

  if (component->selected_pair.keepalive.stun_message.buffer) {
      stun_message_id (&component->selected_pair.keepalive.stun_message,
          conncheck_id);
      if (memcmp (conncheck_id, response_id, sizeof(StunTransactionId)) == 0) {
        nice_debug ("Agent %p : Keepalive for selected pair received.",
            agent);
        if (component->selected_pair.keepalive.tick_source) {
          g_source_destroy (component->selected_pair.keepalive.tick_source);
          g_source_unref (component->selected_pair.keepalive.tick_source);
          component->selected_pair.keepalive.tick_source = NULL;
        }
        component->selected_pair.keepalive.stun_message.buffer = NULL;
        return TRUE;
      }
  }

  return FALSE;
}


typedef struct {
  NiceAgent *agent;
  NiceStream *stream;
  NiceComponent *component;
  uint8_t *password;
} conncheck_validater_data;

static bool conncheck_stun_validater (StunAgent *agent,
    StunMessage *message, uint8_t *username, uint16_t username_len,
    uint8_t **password, size_t *password_len, void *user_data)
{
  conncheck_validater_data *data = (conncheck_validater_data*) user_data;
  GSList *i;
  gchar *ufrag = NULL;
  gsize ufrag_len;

  gboolean msn_msoc_nice_compatibility =
      data->agent->compatibility == NICE_COMPATIBILITY_MSN ||
      data->agent->compatibility == NICE_COMPATIBILITY_OC2007;

  if (data->agent->compatibility == NICE_COMPATIBILITY_OC2007 &&
      stun_message_get_class (message) == STUN_RESPONSE)
    i = data->component->remote_candidates;
  else
    i = data->component->local_candidates;

  for (; i; i = i->next) {
    NiceCandidate *cand = i->data;

    ufrag = NULL;
    if (cand->username)
      ufrag = cand->username;
    else
      ufrag = data->stream->local_ufrag;
    ufrag_len = ufrag? strlen (ufrag) : 0;

    if (ufrag && msn_msoc_nice_compatibility)
      ufrag = (gchar *)g_base64_decode (ufrag, &ufrag_len);

    if (ufrag == NULL)
      continue;

    stun_debug ("Comparing username/ufrag of len %d and %" G_GSIZE_FORMAT ", equal=%d",
        username_len, ufrag_len, username_len >= ufrag_len ?
        memcmp (username, ufrag, ufrag_len) : 0);
    stun_debug_bytes ("  username: ", username, username_len);
    stun_debug_bytes ("  ufrag:    ", ufrag, ufrag_len);
    if (ufrag_len > 0 && username_len >= ufrag_len &&
        memcmp (username, ufrag, ufrag_len) == 0) {
      gchar *pass = NULL;

      if (cand->password)
        pass = cand->password;
      else if (data->stream && data->stream->local_password[0])
        pass = data->stream->local_password;

      if (pass) {
        *password = (uint8_t *) pass;
        *password_len = strlen (pass);

        if (msn_msoc_nice_compatibility) {
          gsize pass_len;

          data->password = g_base64_decode (pass, &pass_len);
          *password = data->password;
          *password_len = pass_len;
        }
      }

      if (msn_msoc_nice_compatibility)
        g_free (ufrag);

      stun_debug ("Found valid username, returning password: '%s'", *password);
      return TRUE;
    }

    if (msn_msoc_nice_compatibility)
      g_free (ufrag);
  }

  return FALSE;
}

/*
 * handle RENOMINATION stun attribute
 * @return TRUE if nomination changed. FALSE otherwise
 */
static gboolean conn_check_handle_renomination (NiceAgent *agent, NiceStream *stream,
    NiceComponent *component, StunMessage *req,
    NiceCandidate *remote_candidate, NiceCandidate *local_candidate)
{
  GSList *lst;
  if (!agent->controlling_mode && NICE_AGENT_IS_COMPATIBLE_WITH_RFC5245_OR_OC2007R2 (agent) &&
      agent->support_renomination && remote_candidate && local_candidate)
  {
    uint32_t nom_value = 0;
    uint16_t nom_len = 0;
    const void *value = stun_message_find (req, STUN_ATTRIBUTE_NOMINATION, &nom_len);
    if (nom_len == 0) {
      return FALSE;
    }
    if (nom_len == 4) {
      memcpy (&nom_value, value, 4);
      nom_value = ntohl (nom_value);
    } else {
      nice_debug ("Agent %p : received NOMINATION attr with incorrect octet length %u, expected 4 bytes",
          agent, nom_len);
      return FALSE;
    }

    if (nice_debug_is_enabled ()) {
      gchar remote_str[INET6_ADDRSTRLEN];
      nice_address_to_string(&remote_candidate->addr, remote_str);
      nice_debug ("Agent %p : received NOMINATION attr for remote candidate [%s]:%u, value is %u",
          agent, remote_str, nice_address_get_port (&remote_candidate->addr), nom_value);
    }

    /*
     * If another pair is SELECTED, change this pair's priority to be greater than
     * selected pair's priority so this pair gets SELECTED!
     */
    if (component->selected_pair.priority &&
        component->selected_pair.remote && component->selected_pair.remote != remote_candidate &&
        component->selected_pair.local && component->selected_pair.local != local_candidate) {
      for (lst = stream->conncheck_list; lst; lst = lst->next) {
        CandidateCheckPair *pair = lst->data;
        if (pair->local == local_candidate && pair->remote == remote_candidate) {
          if (pair->valid) {
            pair->priority = component->selected_pair.priority + 1;
          }
          break;
        }
      }
    }
    priv_mark_pair_nominated (agent, stream, component, local_candidate, remote_candidate);
    return TRUE;
  }
  return FALSE;
}

/*
 * Processing an incoming STUN message.
 *
 * @param agent self pointer
 * @param stream stream the packet is related to
 * @param component component the packet is related to
 * @param nicesock socket from which the packet was received
 * @param from address of the sender
 * @param buf message contents
 * @param buf message length
 *
 * @pre contents of 'buf' is a STUN message
 *
 * @return XXX (what FALSE means exactly?)
 */
gboolean conn_check_handle_inbound_stun (NiceAgent *agent, NiceStream *stream,
    NiceComponent *component, NiceSocket *nicesock, const NiceAddress *from,
    gchar *buf, guint len)
{
  union {
    struct sockaddr_storage storage;
    struct sockaddr addr;
  } sockaddr;
  uint8_t rbuf[MAX_STUN_DATAGRAM_PAYLOAD];
  ssize_t res;
  size_t rbuf_len = sizeof (rbuf);
  bool control = agent->controlling_mode;
  uint8_t uname[NICE_STREAM_MAX_UNAME];
  guint uname_len;
  uint8_t *username;
  uint16_t username_len;
  StunMessage req;
  StunMessage msg;
  StunValidationStatus valid;
  conncheck_validater_data validater_data = {agent, stream, component, NULL};
  GSList *i, *j;
  NiceCandidate *remote_candidate = NULL;
  NiceCandidate *remote_candidate2 = NULL;
  NiceCandidate *local_candidate = NULL;
  gboolean discovery_msg = FALSE;

  nice_address_copy_to_sockaddr (from, &sockaddr.addr);

  /* note: contents of 'buf' already validated, so it is
   *       a valid and fully received STUN message */

  if (nice_debug_is_enabled ()) {
    gchar tmpbuf[INET6_ADDRSTRLEN];
    nice_address_to_string (from, tmpbuf);
    nice_debug ("Agent %p: inbound STUN packet for %u/%u (stream/component) from [%s]:%u (%u octets) :",
        agent, stream->id, component->id, tmpbuf, nice_address_get_port (from), len);
  }

  /* note: ICE  7.2. "STUN Server Procedures" (ID-19) */

  valid = stun_agent_validate (&component->stun_agent, &req,
      (uint8_t *) buf, len, conncheck_stun_validater, &validater_data);

  /* Check for discovery candidates stun agents */
  if (valid == STUN_VALIDATION_BAD_REQUEST ||
      valid == STUN_VALIDATION_UNMATCHED_RESPONSE) {
    for (i = agent->discovery_list; i; i = i->next) {
      CandidateDiscovery *d = i->data;
      if (d->stream_id == stream->id && d->component_id == component->id &&
          d->nicesock == nicesock) {
        valid = stun_agent_validate (&d->stun_agent, &req,
            (uint8_t *) buf, len, conncheck_stun_validater, &validater_data);

        if (valid == STUN_VALIDATION_UNMATCHED_RESPONSE)
          continue;

        discovery_msg = TRUE;
        break;
      }
    }
  }
  /* Check for relay refresh stun agents */
  if (valid == STUN_VALIDATION_BAD_REQUEST ||
      valid == STUN_VALIDATION_UNMATCHED_RESPONSE) {
    for (i = agent->refresh_list; i; i = i->next) {
      CandidateRefresh *r = i->data;

      nice_debug_verbose ("Comparing r.sid=%u to sid=%u, r.cid=%u to cid=%u and %p and %p to %p",
          r->stream_id, stream->id, r->component_id, component->id, r->nicesock,
          r->candidate->sockptr, nicesock);

      if (r->stream_id == stream->id && r->component_id == component->id &&
          (r->nicesock == nicesock || r->candidate->sockptr == nicesock)) {
        valid = stun_agent_validate (&r->stun_agent, &req,
            (uint8_t *) buf, len, conncheck_stun_validater, &validater_data);
        nice_debug ("Validating gave %d", valid);
        if (valid == STUN_VALIDATION_UNMATCHED_RESPONSE)
          continue;
        discovery_msg = TRUE;
        break;
      }
    }
  }

  g_free (validater_data.password);

  if (valid == STUN_VALIDATION_NOT_STUN ||
      valid == STUN_VALIDATION_INCOMPLETE_STUN ||
      valid == STUN_VALIDATION_BAD_REQUEST)
  {
    nice_debug ("Agent %p : Incorrectly multiplexed STUN message ignored.",
        agent);
    return FALSE;
  }

  if (valid == STUN_VALIDATION_UNKNOWN_REQUEST_ATTRIBUTE) {
    nice_debug ("Agent %p : Unknown mandatory attributes in message.", agent);

    if (agent->compatibility != NICE_COMPATIBILITY_MSN &&
        agent->compatibility != NICE_COMPATIBILITY_OC2007) {
      rbuf_len = stun_agent_build_unknown_attributes_error (&component->stun_agent,
          &msg, rbuf, rbuf_len, &req);
      if (rbuf_len != 0)
        agent_socket_send (nicesock, from, rbuf_len, (const gchar*)rbuf);
    }
    return TRUE;
  }

  if (valid == STUN_VALIDATION_UNAUTHORIZED) {
    nice_debug ("Agent %p : Integrity check failed.", agent);

    if (stun_agent_init_error (&component->stun_agent, &msg, rbuf, rbuf_len,
            &req, STUN_ERROR_UNAUTHORIZED)) {
      rbuf_len = stun_agent_finish_message (&component->stun_agent, &msg, NULL, 0);
      if (rbuf_len > 0 && agent->compatibility != NICE_COMPATIBILITY_MSN &&
          agent->compatibility != NICE_COMPATIBILITY_OC2007)
        agent_socket_send (nicesock, from, rbuf_len, (const gchar*)rbuf);
    }
    return TRUE;
  }
  if (valid == STUN_VALIDATION_UNAUTHORIZED_BAD_REQUEST) {
    nice_debug ("Agent %p : Integrity check failed - bad request.", agent);
    if (stun_agent_init_error (&component->stun_agent, &msg, rbuf, rbuf_len,
            &req, STUN_ERROR_BAD_REQUEST)) {
      rbuf_len = stun_agent_finish_message (&component->stun_agent, &msg, NULL, 0);
      if (rbuf_len > 0 && agent->compatibility != NICE_COMPATIBILITY_MSN &&
	  agent->compatibility != NICE_COMPATIBILITY_OC2007)
        agent_socket_send (nicesock, from, rbuf_len, (const gchar*)rbuf);
    }
    return TRUE;
  }

  username = (uint8_t *) stun_message_find (&req, STUN_ATTRIBUTE_USERNAME,
					    &username_len);

  for (i = component->local_candidates; i; i = i->next) {
    NiceCandidate *cand = i->data;
    NiceAddress *addr;

    if (cand->type == NICE_CANDIDATE_TYPE_RELAYED)
      addr = &cand->addr;
    else
      addr = &cand->base_addr;

    if (nice_address_equal (&nicesock->addr, addr) &&
        local_candidate_and_socket_compatible (agent, cand, nicesock)) {
      local_candidate = cand;
      break;
    }
  }

  for (i = component->remote_candidates; i; i = i->next) {
    NiceCandidate *cand = i->data;
    if (nice_address_equal (from, &cand->addr) &&
        remote_candidate_and_socket_compatible (agent, local_candidate,
        cand, nicesock)) {
      remote_candidate = cand;
      break;
    }
  }

  if (agent->compatibility == NICE_COMPATIBILITY_GOOGLE ||
      agent->compatibility == NICE_COMPATIBILITY_MSN ||
      agent->compatibility == NICE_COMPATIBILITY_OC2007) {
    /* We need to find which local candidate was used */
    for (i = component->remote_candidates;
         i != NULL && remote_candidate2 == NULL; i = i->next) {
      for (j = component->local_candidates; j; j = j->next) {
        gboolean inbound = TRUE;
        NiceCandidate *rcand = i->data;
        NiceCandidate *lcand = j->data;

        /* If we receive a response, then the username is local:remote */
        if (agent->compatibility != NICE_COMPATIBILITY_MSN) {
          if (stun_message_get_class (&req) == STUN_REQUEST ||
              stun_message_get_class (&req) == STUN_INDICATION) {
            inbound = TRUE;
          } else {
            inbound = FALSE;
          }
        }

        uname_len = priv_create_username (agent, stream,
            component->id,  rcand, lcand,
            uname, sizeof (uname), inbound);



        stun_debug ("Comparing usernames of size %d and %d: %d",
            username_len, uname_len, username && uname_len == username_len &&
            memcmp (username, uname, uname_len) == 0);
        stun_debug_bytes ("  First username: ", username,
            username ? username_len : 0);
        stun_debug_bytes ("  Second uname:   ", uname, uname_len);

        if (username &&
            uname_len == username_len &&
            memcmp (uname, username, username_len) == 0) {
          local_candidate = lcand;
          remote_candidate2 = rcand;
          break;
        }
      }
    }
  }

  if (component->remote_candidates &&
      agent->compatibility == NICE_COMPATIBILITY_GOOGLE &&
      local_candidate == NULL &&
      discovery_msg == FALSE) {
    /* if we couldn't match the username and the stun agent has
       IGNORE_CREDENTIALS then we have an integrity check failing.
       This could happen with the race condition of receiving connchecks
       before the remote candidates are added. Just drop the message, and let
       the retransmissions make it work. */
    nice_debug ("Agent %p : Username check failed.", agent);
    return TRUE;
  }

  /* This is most likely caused by a second response to a request which
   * already has received a valid reply.
   */
  if (valid == STUN_VALIDATION_UNMATCHED_RESPONSE) {
    nice_debug ("Agent %p : Valid STUN response for which we don't have a request, ignoring", agent);
    return TRUE;
  }

  if (valid != STUN_VALIDATION_SUCCESS) {
    nice_debug ("Agent %p : STUN message is unsuccessful %d, ignoring", agent, valid);
    return FALSE;
  }


  if (stun_message_get_class (&req) == STUN_REQUEST) {
    if (   agent->compatibility == NICE_COMPATIBILITY_MSN
        || agent->compatibility == NICE_COMPATIBILITY_OC2007) {
      if (local_candidate && remote_candidate2) {
        gsize key_len;

	if (agent->compatibility == NICE_COMPATIBILITY_MSN) {
          username = (uint8_t *) stun_message_find (&req,
	  STUN_ATTRIBUTE_USERNAME, &username_len);
	  uname_len = priv_create_username (agent, stream,
              component->id,  remote_candidate2, local_candidate,
	      uname, sizeof (uname), FALSE);
	  memcpy (username, uname, MIN (uname_len, username_len));

	  req.key = g_base64_decode ((gchar *) remote_candidate2->password,
              &key_len);
          req.key_len = key_len;
	} else if (agent->compatibility == NICE_COMPATIBILITY_OC2007) {
          req.key = g_base64_decode ((gchar *) local_candidate->password,
              &key_len);
          req.key_len = key_len;
	}
      } else {
        nice_debug ("Agent %p : received MSN incoming check from unknown remote candidate. "
            "Ignoring request", agent);
        return TRUE;
      }
    }

    rbuf_len = sizeof (rbuf);
    res = stun_usage_ice_conncheck_create_reply (&component->stun_agent, &req,
        &msg, rbuf, &rbuf_len, &sockaddr.storage, sizeof (sockaddr),
        &control, agent->tie_breaker,
        agent_to_ice_compatibility (agent));

    if (   agent->compatibility == NICE_COMPATIBILITY_MSN
        || agent->compatibility == NICE_COMPATIBILITY_OC2007) {
      g_free (req.key);
    }

    if (res == STUN_USAGE_ICE_RETURN_ROLE_CONFLICT)
      priv_check_for_role_conflict (agent, control);

    if (res == STUN_USAGE_ICE_RETURN_SUCCESS ||
        res == STUN_USAGE_ICE_RETURN_ROLE_CONFLICT) {
      /* case 1: valid incoming request, send a reply/error */
      bool use_candidate =
          stun_usage_ice_conncheck_use_candidate (&req);
      uint32_t priority = stun_usage_ice_conncheck_priority (&req);

      if (agent->compatibility == NICE_COMPATIBILITY_GOOGLE ||
          agent->compatibility == NICE_COMPATIBILITY_MSN ||
          agent->compatibility == NICE_COMPATIBILITY_OC2007)
        use_candidate = TRUE;

      if (stream->initial_binding_request_received != TRUE)
        agent_signal_initial_binding_request_received (agent, stream);

      if (remote_candidate == NULL) {
	nice_debug ("Agent %p : No matching remote candidate for incoming "
            "check -> peer-reflexive candidate.", agent);
	remote_candidate = discovery_learn_remote_peer_reflexive_candidate (
            agent, stream, component, priority, from, nicesock,
            local_candidate,
            remote_candidate2 ? remote_candidate2 : remote_candidate);
        if(remote_candidate && stream->remote_ufrag[0]) {
          if (local_candidate &&
              local_candidate->transport == NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE)
            priv_conn_check_add_for_candidate_pair_matched (agent,
                stream->id, component, local_candidate, remote_candidate,
                NICE_CHECK_WAITING);
          else
            conn_check_add_for_candidate (agent, stream->id, component, remote_candidate);
        }
      }

      nice_component_add_valid_candidate (agent, component, remote_candidate);

      priv_reply_to_conn_check (agent, stream, component, local_candidate,
          remote_candidate, from, nicesock, rbuf_len, &msg, use_candidate);

      if (stream->remote_ufrag[0] == 0) {
        /* case: We've got a valid binding request to a local candidate
         *       but we do not yet know remote credentials.
         *       As per sect 7.2 of ICE (ID-19), we send a reply
         *       immediately but postpone all other processing until
         *       we get information about the remote candidates */

        /* step: send a reply immediately but postpone other processing */
        priv_store_pending_check (agent, component, from, nicesock,
            username, username_len, priority, use_candidate);
        priv_print_conn_check_lists (agent, G_STRFUNC, ", icheck stored");
      }
    } else {
      nice_debug ("Agent %p : Invalid STUN packet, ignoring... %s",
          agent, strerror(errno));
      return FALSE;
    }
  } else {
      /* case 2: not a new request, might be a reply...  */
      gboolean trans_found = FALSE;

      /* note: ICE sect 7.1.2. "Processing the Response" (ID-19) */

      /* step: let's try to match the response to an existing check context */
      if (trans_found != TRUE)
        trans_found = priv_map_reply_to_conn_check_request (agent, stream,
	    component, nicesock, from, local_candidate, remote_candidate, &req);

      /* step: let's try to match the response to an existing discovery */
      if (trans_found != TRUE)
        trans_found = priv_map_reply_to_discovery_request (agent, &req);

      /* step: let's try to match the response to an existing turn allocate */
      if (trans_found != TRUE)
        trans_found = priv_map_reply_to_relay_request (agent, &req);

      /* step: let's try to match the response to an existing turn refresh */
      if (trans_found != TRUE)
        trans_found = priv_map_reply_to_relay_refresh (agent, &req);

      if (trans_found != TRUE)
        trans_found = priv_map_reply_to_relay_remove (agent, &req);

      /* step: let's try to match the response to an existing keepalive conncheck */
      if (trans_found != TRUE)
        trans_found = priv_map_reply_to_keepalive_conncheck (agent, component,
            &req);

      if (trans_found != TRUE)
        nice_debug ("Agent %p : Unable to match to an existing transaction, "
            "probably a keepalive.", agent);
  }

  /* RENOMINATION attribute support */
  conn_check_handle_renomination(agent, stream, component, &req, remote_candidate, local_candidate);

  return TRUE;
}

/* Remove all pointers to the given @sock from the connection checking process.
 * These are entirely NiceCandidates pointed to from various places. */
void
conn_check_prune_socket (NiceAgent *agent, NiceStream *stream, NiceComponent *component,
    NiceSocket *sock)
{
  GSList *l;

  if (component->selected_pair.local &&
      component->selected_pair.local->sockptr == sock &&
      component->state == NICE_COMPONENT_STATE_READY) {
    nice_debug ("Agent %p: Selected pair socket %p has been destroyed, "
        "declaring failed", agent, sock);
    agent_signal_component_state_change (agent,
        stream->id, component->id, NICE_COMPONENT_STATE_FAILED);
  }

  /* Prune from the candidate check pairs. */
  for (l = stream->conncheck_list; l != NULL;) {
    CandidateCheckPair *p = l->data;
    GSList *next = l->next;

    if ((p->local != NULL && p->local->sockptr == sock) ||
        (p->remote != NULL && p->remote->sockptr == sock) ||
        (p->sockptr == sock)) {
      nice_debug ("Agent %p : Retransmissions failed, giving up on pair %p",
          agent, p);
      candidate_check_pair_fail (stream, agent, p);
      candidate_check_pair_free (agent, p);
      stream->conncheck_list = g_slist_delete_link (stream->conncheck_list, l);
    }

    l = next;
  }
}
