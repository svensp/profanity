/*
 * message.c
 *
 * Copyright (C) 2012 - 2015 James Booth <boothj5@gmail.com>
 *
 * This file is part of Profanity.
 *
 * Profanity is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Profanity is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Profanity.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link the code of portions of this program with the OpenSSL library under
 * certain conditions as described in each individual source file, and
 * distribute linked combinations including the two.
 *
 * You must obey the GNU General Public License in all respects for all of the
 * code used other than OpenSSL. If you modify file(s) with this exception, you
 * may extend this exception to your version of the file(s), but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version. If you delete this exception statement from all
 * source files in the program, then also delete it here.
 *
 */

#include <stdlib.h>
#include <string.h>

#include <strophe.h>

#include "chat_session.h"
#include "config/preferences.h"
#include "log.h"
#include "muc.h"
#include "profanity.h"
#include "ui/ui.h"
#include "event/server_events.h"
#include "xmpp/connection.h"
#include "xmpp/message.h"
#include "xmpp/roster.h"
#include "roster_list.h"
#include "xmpp/stanza.h"
#include "xmpp/xmpp.h"
#include "pgp/gpg.h"

#define HANDLE(ns, type, func) xmpp_handler_add(conn, func, ns, STANZA_NAME_MESSAGE, type, ctx)

static int _groupchat_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata);
static int _chat_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata);
static int _muc_user_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata);
static int _conference_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata);
static int _captcha_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata);
static int _message_error_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata);
static int _receipt_received_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata);

void
message_add_handlers(void)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();

    HANDLE(NULL,                 STANZA_TYPE_ERROR,      _message_error_handler);
    HANDLE(NULL,                 STANZA_TYPE_GROUPCHAT,  _groupchat_handler);
    HANDLE(NULL,                 NULL,                   _chat_handler);
    HANDLE(STANZA_NS_MUC_USER,   NULL,                   _muc_user_handler);
    HANDLE(STANZA_NS_CONFERENCE, NULL,                   _conference_handler);
    HANDLE(STANZA_NS_CAPTCHA,    NULL,                   _captcha_handler);
    HANDLE(STANZA_NS_RECEIPTS,   NULL,                   _receipt_received_handler);
}

char *
message_send_chat(const char * const barejid, const char * const msg)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();

    ChatSession *session = chat_session_get(barejid);
    char *state = NULL;
    char *jid = NULL;
    if (session) {
        if (prefs_get_boolean(PREF_STATES) && session->send_states) {
            state = STANZA_NAME_ACTIVE;
        }
        Jid *jidp = jid_create_from_bare_and_resource(session->barejid, session->resource);
        jid = strdup(jidp->fulljid);
        jid_destroy(jidp);

    } else {
        if (prefs_get_boolean(PREF_STATES)) {
            state = STANZA_NAME_ACTIVE;
        }
        jid = strdup(barejid);
    }

    char *id = create_unique_id("msg");
    xmpp_stanza_t *message = NULL;

#ifdef HAVE_LIBGPGME
    char *account_name = jabber_get_account_name();
    ProfAccount *account = accounts_get_account(account_name);
    if (account->pgp_keyid) {
        Jid *jidp = jid_create(jid);
        char *encrypted = p_gpg_encrypt(jidp->barejid, msg);
        if (encrypted) {
            message = stanza_create_message(ctx, id, jid, STANZA_TYPE_CHAT, "This message is encrypted.");
            xmpp_stanza_t *x = xmpp_stanza_new(ctx);
            xmpp_stanza_set_name(x, STANZA_NAME_X);
            xmpp_stanza_set_ns(x, STANZA_NS_ENCRYPTED);
            xmpp_stanza_t *enc_st = xmpp_stanza_new(ctx);
            xmpp_stanza_set_text(enc_st, encrypted);
            xmpp_stanza_add_child(x, enc_st);
            xmpp_stanza_release(enc_st);
            xmpp_stanza_add_child(message, x);
            xmpp_stanza_release(x);
            free(encrypted);
        } else {
            message = stanza_create_message(ctx, id, jid, STANZA_TYPE_CHAT, msg);
        }
    } else {
        message = stanza_create_message(ctx, id, jid, STANZA_TYPE_CHAT, msg);
    }
#else
    message = stanza_create_message(ctx, id, jid, STANZA_TYPE_CHAT, msg);
#endif

    free(jid);

    if (state) {
        stanza_attach_state(ctx, message, state);
    }
    if (prefs_get_boolean(PREF_RECEIPTS_REQUEST)) {
        stanza_attach_receipt_request(ctx, message);
    }

    xmpp_send(conn, message);
    xmpp_stanza_release(message);

    return id;
}

char *
message_send_chat_encrypted(const char * const barejid, const char * const msg)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();

    ChatSession *session = chat_session_get(barejid);
    char *state = NULL;
    char *jid = NULL;
    if (session) {
        if (prefs_get_boolean(PREF_STATES) && session->send_states) {
            state = STANZA_NAME_ACTIVE;
        }
        Jid *jidp = jid_create_from_bare_and_resource(session->barejid, session->resource);
        jid = strdup(jidp->fulljid);
        jid_destroy(jidp);
    } else {
        if (prefs_get_boolean(PREF_STATES)) {
            state = STANZA_NAME_ACTIVE;
        }
        jid = strdup(barejid);
    }

    char *id = create_unique_id("msg");
    xmpp_stanza_t *message = stanza_create_message(ctx, id, barejid, STANZA_TYPE_CHAT, msg);
    free(jid);

    if (state) {
        stanza_attach_state(ctx, message, state);
    }
    stanza_attach_carbons_private(ctx, message);
    if (prefs_get_boolean(PREF_RECEIPTS_REQUEST)) {
        stanza_attach_receipt_request(ctx, message);
    }

    xmpp_send(conn, message);
    xmpp_stanza_release(message);

    return id;
}

void
message_send_private(const char * const fulljid, const char * const msg)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    char *id = create_unique_id("prv");
    xmpp_stanza_t *message = stanza_create_message(ctx, id, fulljid, STANZA_TYPE_CHAT, msg);
    free(id);

    xmpp_send(conn, message);
    xmpp_stanza_release(message);
}

void
message_send_groupchat(const char * const roomjid, const char * const msg)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    char *id = create_unique_id("muc");
    xmpp_stanza_t *message = stanza_create_message(ctx, id, roomjid, STANZA_TYPE_GROUPCHAT, msg);
    free(id);

    xmpp_send(conn, message);
    xmpp_stanza_release(message);
}

void
message_send_groupchat_subject(const char * const roomjid, const char * const subject)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *message = stanza_create_room_subject_message(ctx, roomjid, subject);

    xmpp_send(conn, message);
    xmpp_stanza_release(message);
}

void
message_send_invite(const char * const roomjid, const char * const contact,
    const char * const reason)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *stanza;


    muc_member_type_t member_type = muc_member_type(roomjid);
    if (member_type == MUC_MEMBER_TYPE_PUBLIC) {
        log_debug("Sending direct invite to %s, for %s", contact, roomjid);
        char *password = muc_password(roomjid);
        stanza = stanza_create_invite(ctx, roomjid, contact, reason, password);
    } else {
        log_debug("Sending mediated invite to %s, for %s", contact, roomjid);
        stanza = stanza_create_mediated_invite(ctx, roomjid, contact, reason);
    }

    xmpp_send(conn, stanza);
    xmpp_stanza_release(stanza);
}

void
message_send_composing(const char * const jid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();

    xmpp_stanza_t *stanza = stanza_create_chat_state(ctx, jid, STANZA_NAME_COMPOSING);
    xmpp_send(conn, stanza);
    xmpp_stanza_release(stanza);

}

void
message_send_paused(const char * const jid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *stanza = stanza_create_chat_state(ctx, jid, STANZA_NAME_PAUSED);
    xmpp_send(conn, stanza);
    xmpp_stanza_release(stanza);
}

void
message_send_inactive(const char * const jid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *stanza = stanza_create_chat_state(ctx, jid, STANZA_NAME_INACTIVE);

    xmpp_send(conn, stanza);
    xmpp_stanza_release(stanza);
}

void
message_send_gone(const char * const jid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *stanza = stanza_create_chat_state(ctx, jid, STANZA_NAME_GONE);
    xmpp_send(conn, stanza);
    xmpp_stanza_release(stanza);
}

static int
_message_error_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata)
{
    char *id = xmpp_stanza_get_id(stanza);
    char *jid = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    xmpp_stanza_t *error_stanza = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_ERROR);
    char *type = NULL;
    if (error_stanza) {
        type = xmpp_stanza_get_attribute(error_stanza, STANZA_ATTR_TYPE);
    }

    // stanza_get_error never returns NULL
    char *err_msg = stanza_get_error_message(stanza);

    GString *log_msg = g_string_new("message stanza error received");
    if (id) {
        g_string_append(log_msg, " id=");
        g_string_append(log_msg, id);
    }
    if (jid) {
        g_string_append(log_msg, " from=");
        g_string_append(log_msg, jid);
    }
    if (type) {
        g_string_append(log_msg, " type=");
        g_string_append(log_msg, type);
    }
    g_string_append(log_msg, " error=");
    g_string_append(log_msg, err_msg);

    log_info(log_msg->str);

    g_string_free(log_msg, TRUE);

    if (!jid) {
        ui_handle_error(err_msg);
    } else if (type && (strcmp(type, "cancel") == 0)) {
        log_info("Recipient %s not found: %s", jid, err_msg);
        Jid *jidp = jid_create(jid);
        chat_session_remove(jidp->barejid);
        jid_destroy(jidp);
    } else {
        ui_handle_recipient_error(jid, err_msg);
    }

    free(err_msg);

    return 1;
}

static int
_muc_user_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata)
{
    xmpp_ctx_t *ctx = connection_get_ctx();
    xmpp_stanza_t *xns_muc_user = xmpp_stanza_get_child_by_ns(stanza, STANZA_NS_MUC_USER);
    char *room = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);

    if (!room) {
        log_warning("Message received with no from attribute, ignoring");
        return 1;
    }

    // XEP-0045
    xmpp_stanza_t *invite = xmpp_stanza_get_child_by_name(xns_muc_user, STANZA_NAME_INVITE);
    if (!invite) {
        return 1;
    }

    char *invitor_jid = xmpp_stanza_get_attribute(invite, STANZA_ATTR_FROM);
    if (!invitor_jid) {
        log_warning("Chat room invite received with no from attribute");
        return 1;
    }

    Jid *jidp = jid_create(invitor_jid);
    if (!jidp) {
        return 1;
    }
    char *invitor = jidp->barejid;

    char *reason = NULL;
    xmpp_stanza_t *reason_st = xmpp_stanza_get_child_by_name(invite, STANZA_NAME_REASON);
    if (reason_st) {
        reason = xmpp_stanza_get_text(reason_st);
    }

    char *password = NULL;
    xmpp_stanza_t *password_st = xmpp_stanza_get_child_by_name(xns_muc_user, STANZA_NAME_PASSWORD);
    if (password_st) {
        password = xmpp_stanza_get_text(password_st);
    }

    sv_ev_room_invite(INVITE_MEDIATED, invitor, room, reason, password);
    jid_destroy(jidp);
    if (reason) {
        xmpp_free(ctx, reason);
    }
    if (password) {
        xmpp_free(ctx, password);
    }

    return 1;
}

static int
_conference_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata)
{
    xmpp_stanza_t *xns_conference = xmpp_stanza_get_child_by_ns(stanza, STANZA_NS_CONFERENCE);

    char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    if (!from) {
        log_warning("Message received with no from attribute, ignoring");
        return 1;
    }

    Jid *jidp = jid_create(from);
    if (!jidp) {
        return 1;
    }

    // XEP-0249
    char *room = xmpp_stanza_get_attribute(xns_conference, STANZA_ATTR_JID);
    if (!room) {
        return 1;
    }

    char *reason = xmpp_stanza_get_attribute(xns_conference, STANZA_ATTR_REASON);
    char *password = xmpp_stanza_get_attribute(xns_conference, STANZA_ATTR_PASSWORD);

    sv_ev_room_invite(INVITE_DIRECT, jidp->barejid, room, reason, password);
    jid_destroy(jidp);

    return 1;
}

static int
_captcha_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata)
{
    xmpp_ctx_t *ctx = connection_get_ctx();
    char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);

    if (!from) {
        log_warning("Message received with no from attribute, ignoring");
        return 1;
    }

    // XEP-0158
    xmpp_stanza_t *body = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_BODY);
    if (!body) {
        return 1;
    }

    char *message = xmpp_stanza_get_text(body);
    if (!message) {
        return 1;
    }

    sv_ev_room_broadcast(from, message);
    xmpp_free(ctx, message);

    return 1;
}

static int
_groupchat_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata)
{
    xmpp_ctx_t *ctx = connection_get_ctx();
    char *message = NULL;
    char *room_jid = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    Jid *jid = jid_create(room_jid);

    // handle room subject
    xmpp_stanza_t *subject = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_SUBJECT);
    if (subject) {
        message = xmpp_stanza_get_text(subject);
        sv_ev_room_subject(jid->barejid, jid->resourcepart, message);
        xmpp_free(ctx, message);

        jid_destroy(jid);
        return 1;
    }

    // handle room broadcasts
    if (!jid->resourcepart) {
        xmpp_stanza_t *body = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_BODY);
        if (!body) {
            jid_destroy(jid);
            return 1;
        }

        message = xmpp_stanza_get_text(body);
        if (!message) {
            jid_destroy(jid);
            return 1;
        }

        sv_ev_room_broadcast(room_jid, message);
        xmpp_free(ctx, message);

        jid_destroy(jid);
        return 1;
    }

    if (!jid_is_valid_room_form(jid)) {
        log_error("Invalid room JID: %s", jid->str);
        jid_destroy(jid);
        return 1;
    }

    // room not active in profanity
    if (!muc_active(jid->barejid)) {
        log_error("Message received for inactive chat room: %s", jid->str);
        jid_destroy(jid);
        return 1;
    }

    xmpp_stanza_t *body = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_BODY);

    // check for and deal with message
    if (!body) {
        jid_destroy(jid);
        return 1;
    }

    message = xmpp_stanza_get_text(body);
    if (!message) {
        jid_destroy(jid);
        return 1;
    }

    // determine if the notifications happened whilst offline
    GTimeVal tv_stamp;
    gboolean delayed = stanza_get_delay(stanza, &tv_stamp);
    if (delayed) {
        sv_ev_room_history(jid->barejid, jid->resourcepart, tv_stamp, message);
    } else {
        sv_ev_room_message(jid->barejid, jid->resourcepart, message);
    }

    xmpp_free(ctx, message);
    jid_destroy(jid);

    return 1;
}

void
_message_send_receipt(const char * const fulljid, const char * const message_id)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *message = xmpp_stanza_new(ctx);
    char *id = create_unique_id("receipt");
    xmpp_stanza_set_name(message, STANZA_NAME_MESSAGE);
    xmpp_stanza_set_id(message, id);
    xmpp_stanza_set_attribute(message, STANZA_ATTR_TO, fulljid);

    xmpp_stanza_t *receipt = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(receipt, "received");
    xmpp_stanza_set_ns(receipt, STANZA_NS_RECEIPTS);
    xmpp_stanza_set_attribute(receipt, STANZA_ATTR_ID, message_id);

    xmpp_stanza_add_child(message, receipt);
    xmpp_stanza_release(receipt);

    xmpp_send(conn, message);
    xmpp_stanza_release(message);
}

static int
_receipt_received_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata)
{
    xmpp_stanza_t *receipt = xmpp_stanza_get_child_by_ns(stanza, STANZA_NS_RECEIPTS);
    char *name = xmpp_stanza_get_name(receipt);
    if (g_strcmp0(name, "received") != 0) {
        return 1;
    }

    char *id = xmpp_stanza_get_attribute(receipt, STANZA_ATTR_ID);
    if (!id) {
        return 1;
    }

    char *fulljid = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    if (!fulljid) {
        return 1;
    }

    Jid *jidp = jid_create(fulljid);
    sv_ev_message_receipt(jidp->barejid, id);
    jid_destroy(jidp);

    return 1;
}

void
_receipt_request_handler(xmpp_stanza_t * const stanza)
{
    if (!prefs_get_boolean(PREF_RECEIPTS_SEND)) {
        return;
    }

    char *id = xmpp_stanza_get_id(stanza);
    if (!id) {
        return;
    }

    xmpp_stanza_t *receipts = xmpp_stanza_get_child_by_ns(stanza, STANZA_NS_RECEIPTS);
    if (!receipts) {
        return;
    }

    char *receipts_name = xmpp_stanza_get_name(receipts);
    if (g_strcmp0(receipts_name, "request") != 0) {
        return;
    }

    gchar *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    Jid *jid = jid_create(from);
    _message_send_receipt(jid->fulljid, id);
    jid_destroy(jid);
}

void
_private_chat_handler(xmpp_stanza_t * const stanza, const char * const fulljid)
{
    xmpp_stanza_t *body = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_BODY);
    if (!body) {
        return;
    }

    char *message = xmpp_stanza_get_text(body);
    if (!message) {
        return;
    }

    GTimeVal tv_stamp;
    gboolean delayed = stanza_get_delay(stanza, &tv_stamp);
    if (delayed) {
        sv_ev_delayed_private_message(fulljid, message, tv_stamp);
    } else {
        sv_ev_incoming_private_message(fulljid, message);
    }

    xmpp_ctx_t *ctx = connection_get_ctx();
    xmpp_free(ctx, message);
}

static int
_chat_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata)
{
    // ignore if type not chat or absent
    char *type = xmpp_stanza_get_type(stanza);
    if (!(g_strcmp0(type, "chat") == 0 || type == NULL)) {
        return 1;
    }

    // check if carbon message
    xmpp_stanza_t *carbons = xmpp_stanza_get_child_by_ns(stanza, STANZA_NS_CARBONS);
    if (carbons) {
        char *name = xmpp_stanza_get_name(carbons);
        if ((g_strcmp0(name, "received") == 0) || (g_strcmp0(name, "sent")) == 0){
            xmpp_stanza_t *forwarded = xmpp_stanza_get_child_by_ns(carbons, STANZA_NS_FORWARD);
            xmpp_stanza_t *message = xmpp_stanza_get_child_by_name(forwarded, STANZA_NAME_MESSAGE);

            xmpp_ctx_t *ctx = connection_get_ctx();

            gchar *to = xmpp_stanza_get_attribute(message, STANZA_ATTR_TO);
            gchar *from = xmpp_stanza_get_attribute(message, STANZA_ATTR_FROM);

            // happens when receive a carbon of a self sent message
            if (!to) to = from;

            Jid *jid_from = jid_create(from);
            Jid *jid_to = jid_create(to);
            Jid *my_jid = jid_create(jabber_get_fulljid());

            // check for and deal with message
            xmpp_stanza_t *body = xmpp_stanza_get_child_by_name(message, STANZA_NAME_BODY);
            if (body) {
                char *message = xmpp_stanza_get_text(body);
                if (message) {
                    // if we are the recipient, treat as standard incoming message
                    if(g_strcmp0(my_jid->barejid, jid_to->barejid) == 0){
                        sv_ev_incoming_message(jid_from->barejid, jid_from->resourcepart, message);
                    }
                    // else treat as a sent message
                    else{
                        sv_ev_carbon(jid_to->barejid, message);
                    }
                    xmpp_free(ctx, message);
                }
            }

            jid_destroy(jid_from);
            jid_destroy(jid_to);
            jid_destroy(my_jid);
            return 1;
        }
    }

    // ignore handled namespaces
    xmpp_stanza_t *conf = xmpp_stanza_get_child_by_ns(stanza, STANZA_NS_CONFERENCE);
    xmpp_stanza_t *mucuser = xmpp_stanza_get_child_by_ns(stanza, STANZA_NS_MUC_USER);
    xmpp_stanza_t *captcha = xmpp_stanza_get_child_by_ns(stanza, STANZA_NS_CAPTCHA);
    if (conf || mucuser || captcha) {
        return 1;
    }

    gchar *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    Jid *jid = jid_create(from);

    // private message from chat room use full jid (room/nick)
    if (muc_active(jid->barejid)) {
        _private_chat_handler(stanza, jid->fulljid);
        return 1;
    }

    // standard chat message, use jid without resource
    GTimeVal tv_stamp;
    gboolean delayed = stanza_get_delay(stanza, &tv_stamp);

    xmpp_stanza_t *body = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_BODY);
    if (body) {
        char *message = xmpp_stanza_get_text(body);
        if (message) {
            if (delayed) {
                sv_ev_delayed_message(jid->barejid, message, tv_stamp);
            } else {
#ifdef HAVE_LIBGPGME
                gboolean handled = FALSE;
                xmpp_stanza_t *x = xmpp_stanza_get_child_by_ns(stanza, STANZA_NS_ENCRYPTED);
                if (x) {
                    char *enc_message = xmpp_stanza_get_text(x);
                    char *decrypted = p_gpg_decrypt(jid->barejid, enc_message);
                    if (decrypted) {
                        sv_ev_incoming_message(jid->barejid, jid->resourcepart, decrypted);
                        handled = TRUE;
                    }
                }
                if (!handled) {
                    sv_ev_incoming_message(jid->barejid, jid->resourcepart, message);
                }
#else
                sv_ev_incoming_message(jid->barejid, jid->resourcepart, message);
#endif
            }

            _receipt_request_handler(stanza);

            xmpp_ctx_t *ctx = connection_get_ctx();
            xmpp_free(ctx, message);
        }
    }

    // handle chat sessions and states
    if (!delayed && jid->resourcepart) {
        gboolean gone = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_GONE) != NULL;
        gboolean typing = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_COMPOSING) != NULL;
        gboolean paused = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_PAUSED) != NULL;
        gboolean inactive = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_INACTIVE) != NULL;
        if (gone) {
            sv_ev_gone(jid->barejid, jid->resourcepart);
        } else if (typing) {
            sv_ev_typing(jid->barejid, jid->resourcepart);
        } else if (paused) {
            sv_ev_paused(jid->barejid, jid->resourcepart);
        } else if (inactive) {
            sv_ev_inactive(jid->barejid, jid->resourcepart);
        } else if (stanza_contains_chat_state(stanza)) {
            sv_ev_activity(jid->barejid, jid->resourcepart, TRUE);
        } else {
            sv_ev_activity(jid->barejid, jid->resourcepart, FALSE);
        }
    }

    jid_destroy(jid);
    return 1;
}
