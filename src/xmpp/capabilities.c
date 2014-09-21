/*
 * capabilities.c
 *
 * Copyright (C) 2012 - 2014 James Booth <boothj5@gmail.com>
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

#include "config.h"

#ifdef HAVE_GIT_VERSION
#include "gitversion.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <strophe.h>

#include "common.h"
#include "log.h"
#include "xmpp/xmpp.h"
#include "xmpp/stanza.h"
#include "xmpp/form.h"

static GHashTable *capabilities;
static GHashTable *jid_lookup;

static void _caps_destroy(Capabilities *caps);

void
caps_init(void)
{
    capabilities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
        (GDestroyNotify)_caps_destroy);
    jid_lookup = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

void
caps_add(const char * const ver, Capabilities *caps)
{
    g_hash_table_insert(capabilities, strdup(ver), caps);
}

void
caps_map(const char * const jid, const char * const ver)
{
    g_hash_table_insert(jid_lookup, strdup(jid), strdup(ver));
}

gboolean
caps_contains(const char * const caps_ver)
{
    return (g_hash_table_lookup(capabilities, caps_ver) != NULL);
}

static Capabilities *
_caps_get(const char * const caps_str)
{
    return g_hash_table_lookup(capabilities, caps_str);
}

static Capabilities *
_caps_lookup(const char * const jid)
{
    char *ver = g_hash_table_lookup(jid_lookup, jid);
    if (ver) {
        Capabilities *caps = g_hash_table_lookup(capabilities, ver);
        if (caps) {
            return caps;
        }
    }

    return NULL;
}

char *
caps_create_sha1_str(xmpp_stanza_t * const query)
{
    char *category = NULL;
    char *type = NULL;
    char *lang = NULL;
    char *name = NULL;
    char *feature_str = NULL;
    GSList *identities = NULL;
    GSList *features = NULL;
    GSList *form_names = NULL;
    DataForm *form = NULL;
    FormField *field = NULL;
    GHashTable *forms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)form_destroy);


    xmpp_stanza_t *child = xmpp_stanza_get_children(query);
    while (child) {
        if (g_strcmp0(xmpp_stanza_get_name(child), STANZA_NAME_IDENTITY) == 0) {
            category = xmpp_stanza_get_attribute(child, "category");
            type = xmpp_stanza_get_attribute(child, "type");
            lang = xmpp_stanza_get_attribute(child, "xml:lang");
            name = xmpp_stanza_get_attribute(child, "name");

            GString *identity_str = g_string_new(category);
            g_string_append(identity_str, "/");
            if (type) {
                g_string_append(identity_str, type);
            }
            g_string_append(identity_str, "/");
            if (lang) {
                g_string_append(identity_str, lang);
            }
            g_string_append(identity_str, "/");
            if (name) {
                g_string_append(identity_str, name);
            }
            g_string_append(identity_str, "<");
            identities = g_slist_insert_sorted(identities, g_strdup(identity_str->str), (GCompareFunc)strcmp);
            g_string_free(identity_str, TRUE);
        } else if (g_strcmp0(xmpp_stanza_get_name(child), STANZA_NAME_FEATURE) == 0) {
            feature_str = xmpp_stanza_get_attribute(child, "var");
            features = g_slist_insert_sorted(features, g_strdup(feature_str), (GCompareFunc)strcmp);
        } else if (g_strcmp0(xmpp_stanza_get_name(child), STANZA_NAME_X) == 0) {
            if (g_strcmp0(xmpp_stanza_get_ns(child), STANZA_NS_DATA) == 0) {
                form = form_create(child);
                char *form_type = form_get_form_type_field(form);
                form_names = g_slist_insert_sorted(form_names, g_strdup(form_type), (GCompareFunc)strcmp);
                g_hash_table_insert(forms, g_strdup(form_type), form);
            }
        }
        child = xmpp_stanza_get_next(child);
    }

    GString *s = g_string_new("");

    GSList *curr = identities;
    while (curr) {
        g_string_append(s, curr->data);
        curr = g_slist_next(curr);
    }

    curr = features;
    while (curr) {
        g_string_append(s, curr->data);
        g_string_append(s, "<");
        curr = g_slist_next(curr);
    }

    curr = form_names;
    while (curr) {
        form = g_hash_table_lookup(forms, curr->data);
        char *form_type = form_get_form_type_field(form);
        g_string_append(s, form_type);
        g_string_append(s, "<");

        GSList *sorted_fields = form_get_non_form_type_fields_sorted(form);
        GSList *curr_field = sorted_fields;
        while (curr_field) {
            field = curr_field->data;
            g_string_append(s, field->var);
            g_string_append(s, "<");

            GSList *sorted_values = form_get_field_values_sorted(field);
            GSList *curr_value = sorted_values;
            while (curr_value) {
                g_string_append(s, curr_value->data);
                g_string_append(s, "<");
                curr_value = g_slist_next(curr_value);
            }
            g_slist_free(sorted_values);
            curr_field = g_slist_next(curr_field);
        }
        g_slist_free(sorted_fields);

        curr = g_slist_next(curr);
    }

    log_debug("Generating capabilities hash for: %s", s->str);
    char *result = p_sha1_hash(s->str);
    log_debug("Hash: %s", result);

    g_string_free(s, TRUE);
    g_slist_free_full(identities, g_free);
    g_slist_free_full(features, g_free);
    g_slist_free_full(form_names, g_free);
    g_hash_table_destroy(forms);

    return result;
}

Capabilities *
caps_create(xmpp_stanza_t *query)
{
    const char *category = NULL;
    const char *type = NULL;
    const char *name = NULL;
    const char *software = NULL;
    const char *software_version = NULL;
    const char *os = NULL;
    const char *os_version = NULL;
    GSList *features = NULL;

    xmpp_stanza_t *identity = xmpp_stanza_get_child_by_name(query, "identity");
    if (identity != NULL) {
        category = xmpp_stanza_get_attribute(identity, "category");
        type = xmpp_stanza_get_attribute(identity, "type");
        name = xmpp_stanza_get_attribute(identity, "name");
    }

    xmpp_stanza_t *softwareinfo = xmpp_stanza_get_child_by_ns(query, STANZA_NS_DATA);
    if (softwareinfo != NULL) {
        DataForm *form = form_create(softwareinfo);
        FormField *formField = NULL;

        char *form_type = form_get_form_type_field(form);
        if (g_strcmp0(form_type, STANZA_DATAFORM_SOFTWARE) == 0) {
            GSList *field = form->fields;
            while (field != NULL) {
                formField = field->data;
                if (formField->values != NULL) {
                    if (strcmp(formField->var, "software") == 0) {
                        software = formField->values->data;
                    } else if (strcmp(formField->var, "software_version") == 0) {
                        software_version = formField->values->data;
                    } else if (strcmp(formField->var, "os") == 0) {
                        os = formField->values->data;
                    } else if (strcmp(formField->var, "os_version") == 0) {
                        os_version = formField->values->data;
                    }
                }
                field = g_slist_next(field);
            }
        }

        form_destroy(form);
    }

    xmpp_stanza_t *child = xmpp_stanza_get_children(query);
    while (child != NULL) {
        if (g_strcmp0(xmpp_stanza_get_name(child), "feature") == 0) {
            features = g_slist_append(features, strdup(xmpp_stanza_get_attribute(child, "var")));
        }

        child = xmpp_stanza_get_next(child);
    }

    Capabilities *new_caps = malloc(sizeof(struct capabilities_t));

    if (category != NULL) {
        new_caps->category = strdup(category);
    } else {
        new_caps->category = NULL;
    }
    if (type != NULL) {
        new_caps->type = strdup(type);
    } else {
        new_caps->type = NULL;
    }
    if (name != NULL) {
        new_caps->name = strdup(name);
    } else {
        new_caps->name = NULL;
    }
    if (software != NULL) {
        new_caps->software = strdup(software);
    } else {
        new_caps->software = NULL;
    }
    if (software_version != NULL) {
        new_caps->software_version = strdup(software_version);
    } else {
        new_caps->software_version = NULL;
    }
    if (os != NULL) {
        new_caps->os = strdup(os);
    } else {
        new_caps->os = NULL;
    }
    if (os_version != NULL) {
        new_caps->os_version = strdup(os_version);
    } else {
        new_caps->os_version = NULL;
    }
    if (features != NULL) {
        new_caps->features = features;
    } else {
        new_caps->features = NULL;
    }

    return new_caps;
}


xmpp_stanza_t *
caps_create_query_response_stanza(xmpp_ctx_t * const ctx)
{
    xmpp_stanza_t *query = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(query, STANZA_NAME_QUERY);
    xmpp_stanza_set_ns(query, XMPP_NS_DISCO_INFO);

    xmpp_stanza_t *identity = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(identity, "identity");
    xmpp_stanza_set_attribute(identity, "category", "client");
    xmpp_stanza_set_attribute(identity, "type", "console");

    GString *name_str = g_string_new("Profanity ");
    g_string_append(name_str, PACKAGE_VERSION);
    if (strcmp(PACKAGE_STATUS, "development") == 0) {
#ifdef HAVE_GIT_VERSION
        g_string_append(name_str, "dev.");
        g_string_append(name_str, PROF_GIT_BRANCH);
        g_string_append(name_str, ".");
        g_string_append(name_str, PROF_GIT_REVISION);
#else
        g_string_append(name_str, "dev");
#endif
    }
    xmpp_stanza_set_attribute(identity, "name", name_str->str);
    g_string_free(name_str, TRUE);

    xmpp_stanza_t *feature_caps = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(feature_caps, STANZA_NAME_FEATURE);
    xmpp_stanza_set_attribute(feature_caps, STANZA_ATTR_VAR, STANZA_NS_CAPS);

    xmpp_stanza_t *feature_discoinfo = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(feature_discoinfo, STANZA_NAME_FEATURE);
    xmpp_stanza_set_attribute(feature_discoinfo, STANZA_ATTR_VAR, XMPP_NS_DISCO_INFO);

    xmpp_stanza_t *feature_discoitems = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(feature_discoitems, STANZA_NAME_FEATURE);
    xmpp_stanza_set_attribute(feature_discoitems, STANZA_ATTR_VAR, XMPP_NS_DISCO_ITEMS);

    xmpp_stanza_t *feature_muc = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(feature_muc, STANZA_NAME_FEATURE);
    xmpp_stanza_set_attribute(feature_muc, STANZA_ATTR_VAR, STANZA_NS_MUC);

    xmpp_stanza_t *feature_version = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(feature_version, STANZA_NAME_FEATURE);
    xmpp_stanza_set_attribute(feature_version, STANZA_ATTR_VAR, STANZA_NS_VERSION);

    xmpp_stanza_t *feature_chatstates = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(feature_chatstates, STANZA_NAME_FEATURE);
    xmpp_stanza_set_attribute(feature_chatstates, STANZA_ATTR_VAR, STANZA_NS_CHATSTATES);

    xmpp_stanza_t *feature_ping = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(feature_ping, STANZA_NAME_FEATURE);
    xmpp_stanza_set_attribute(feature_ping, STANZA_ATTR_VAR, STANZA_NS_PING);

    xmpp_stanza_add_child(query, identity);

    xmpp_stanza_add_child(query, feature_caps);
    xmpp_stanza_add_child(query, feature_chatstates);
    xmpp_stanza_add_child(query, feature_discoinfo);
    xmpp_stanza_add_child(query, feature_discoitems);
    xmpp_stanza_add_child(query, feature_muc);
    xmpp_stanza_add_child(query, feature_version);
    xmpp_stanza_add_child(query, feature_ping);

    xmpp_stanza_release(feature_ping);
    xmpp_stanza_release(feature_version);
    xmpp_stanza_release(feature_muc);
    xmpp_stanza_release(feature_discoitems);
    xmpp_stanza_release(feature_discoinfo);
    xmpp_stanza_release(feature_chatstates);
    xmpp_stanza_release(feature_caps);
    xmpp_stanza_release(identity);

    return query;
}

static void
_caps_close(void)
{
    g_hash_table_destroy(capabilities);
    g_hash_table_destroy(jid_lookup);
}

static void
_caps_destroy(Capabilities *caps)
{
    if (caps != NULL) {
        free(caps->category);
        free(caps->type);
        free(caps->name);
        free(caps->software);
        free(caps->software_version);
        free(caps->os);
        free(caps->os_version);
        if (caps->features != NULL) {
            g_slist_free_full(caps->features, free);
        }
        free(caps);
    }
}

void
capabilities_init_module(void)
{
    caps_get = _caps_get;
    caps_lookup = _caps_lookup;
    caps_close = _caps_close;
}
