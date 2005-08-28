/* $Id$ */

/***
  This file is part of avahi.
 
  avahi is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  avahi is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
  Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with avahi; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <dbus/dbus.h>

#include <avahi-client/client.h>
#include <avahi-common/dbus.h>
#include <avahi-common/llist.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>

#include "client.h"
#include "internal.h"

/* AvahiServiceResolver implementation */

DBusHandlerResult avahi_service_resolver_event (AvahiClient *client, AvahiResolverEvent event, DBusMessage *message) {
    AvahiServiceResolver *r = NULL;
    DBusError error;
    const char *path;
    AvahiStringList *strlst = NULL;

    assert(client);
    assert(message);
    
    dbus_error_init (&error);

    if (!(path = dbus_message_get_path(message)))
        goto fail;

    for (r = client->service_resolvers; r; r = r->service_resolvers_next)
        if (strcmp (r->path, path) == 0)
            break;

    if (!r)
        goto fail;

    if (event == AVAHI_RESOLVER_FOUND) {
        int j;
        int32_t interface;
        AvahiProtocol protocol, aprotocol;
        char *name, *type, *domain, *host, *address;
        uint16_t port;
        DBusMessageIter iter, sub;
        AvahiAddress a;
        
        if (!dbus_message_get_args(
                message, &error,
                DBUS_TYPE_INT32, &interface,
                DBUS_TYPE_INT32, &protocol,
                DBUS_TYPE_STRING, &name,
                DBUS_TYPE_STRING, &type,
                DBUS_TYPE_STRING, &domain,
                DBUS_TYPE_STRING, &host,
                DBUS_TYPE_INT32, &aprotocol,
                DBUS_TYPE_STRING, &address,
                DBUS_TYPE_UINT16, &port,
                DBUS_TYPE_INVALID) ||
            dbus_error_is_set (&error)) {
            fprintf(stderr, "Failed to parse resolver event.\n");
            goto fail;
        }
        
        dbus_message_iter_init(message, &iter);
        
        for (j = 0; j < 9; j++)
            dbus_message_iter_next(&iter);
        
        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_ARRAY) {
            fprintf(stderr, "Error parsing service resolving message");
            goto fail;
        }
        
        strlst = NULL;
        dbus_message_iter_recurse(&iter, &sub);
        
        for (;;) {
            DBusMessageIter sub2;
            int at;
            
            if ((at = dbus_message_iter_get_arg_type(&sub)) == DBUS_TYPE_INVALID)
                break;
            
            assert(at == DBUS_TYPE_ARRAY);
            
            if (dbus_message_iter_get_element_type(&sub) != DBUS_TYPE_BYTE) {
                fprintf(stderr, "Error parsing service resolving message");
                goto fail;
            }
            
            dbus_message_iter_recurse(&sub, &sub2);

            if (dbus_message_iter_get_array_len(&sub2) > 0) {
                uint8_t *k;
                int n;
                
                dbus_message_iter_get_fixed_array(&sub2, &k, &n);
                strlst = avahi_string_list_add_arbitrary(strlst, k, n);
            }
            
            dbus_message_iter_next(&sub);
        }

        assert(address);
        if (!avahi_address_parse(address, (AvahiProtocol) aprotocol, &a)) {
            fprintf(stderr, "Failed to parse address\n");
            goto fail;
        }
    
        r->callback(r, (AvahiIfIndex) interface, (AvahiProtocol) protocol, AVAHI_RESOLVER_FOUND, name, type, domain, host, &a, port, strlst, r->userdata);
        
        avahi_string_list_free(strlst);

    } else {
        assert(event == AVAHI_RESOLVER_TIMEOUT);
        
        r->callback(r, (AvahiIfIndex) 0, (AvahiProtocol) 0, AVAHI_RESOLVER_TIMEOUT, NULL, NULL, NULL, NULL, NULL, 0, NULL, r->userdata);
    }

    return DBUS_HANDLER_RESULT_HANDLED;

    
fail:
    dbus_error_free (&error);
    avahi_string_list_free(strlst);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


AvahiServiceResolver * avahi_service_resolver_new(
    AvahiClient *client,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    const char *name,
    const char *type,
    const char *domain,
    AvahiProtocol aprotocol,
    AvahiServiceResolverCallback callback,
    void *userdata) {

    DBusError error;
    AvahiServiceResolver *r;
    DBusMessage *message = NULL, *reply = NULL;
    int32_t i_interface, i_protocol, i_aprotocol;
    char *path;
    
    assert(client);
    assert(name);
    assert(type);

    if (!domain)
        domain = "";
    
    dbus_error_init (&error);

    if (client->state == AVAHI_CLIENT_DISCONNECTED) {
        avahi_client_set_errno(client, AVAHI_ERR_BAD_STATE);
        goto fail;
    }

    if (!(r = avahi_new(AvahiServiceResolver, 1))) {
        avahi_client_set_errno(client, AVAHI_ERR_NO_MEMORY);
        goto fail;
    }

    r->client = client;
    r->callback = callback;
    r->userdata = userdata;
    r->path = NULL;
    
    AVAHI_LLIST_PREPEND(AvahiServiceResolver, service_resolvers, client->service_resolvers, r);

    if (!(message = dbus_message_new_method_call(AVAHI_DBUS_NAME, AVAHI_DBUS_PATH_SERVER, AVAHI_DBUS_INTERFACE_SERVER, "ServiceResolverNew"))) {
        avahi_client_set_errno(client, AVAHI_ERR_NO_MEMORY);
        goto fail;
    }

    i_interface = interface;
    i_protocol = protocol;
    i_aprotocol = aprotocol;

    if (!(dbus_message_append_args(
              message,
              DBUS_TYPE_INT32, &i_interface,
              DBUS_TYPE_INT32, &i_protocol,
              DBUS_TYPE_STRING, &name,
              DBUS_TYPE_STRING, &type,
              DBUS_TYPE_STRING, &domain,
              DBUS_TYPE_INT32, &i_aprotocol,
              DBUS_TYPE_INVALID))) {
        avahi_client_set_errno(client, AVAHI_ERR_NO_MEMORY);
        goto fail;
    }

    if (!(reply = dbus_connection_send_with_reply_and_block(client->bus, message, -1, &error)) ||
        dbus_error_is_set(&error)) {
        avahi_client_set_errno(client, AVAHI_ERR_DBUS_ERROR);
        goto fail;
    }

    if (!dbus_message_get_args (reply, &error, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID) ||
        dbus_error_is_set(&error) ||
        !path) {
        avahi_client_set_errno(client, AVAHI_ERR_DBUS_ERROR);
        goto fail;
    }

    if (!(r->path = avahi_strdup(path))) {

        /* FIXME: We don't remove the object on the server side */

        avahi_client_set_errno(client, AVAHI_ERR_NO_MEMORY);
        goto fail;
    }
        

    dbus_message_unref(message);
    dbus_message_unref(reply);

    return r;
    
fail:

    if (dbus_error_is_set(&error)) {
        avahi_client_set_dbus_error(client, &error);
        dbus_error_free(&error);
    }

    if (r)
        avahi_service_resolver_free(r);
    
    if (message)
        dbus_message_unref(message);

    if (reply)
        dbus_message_unref(reply);
    
    return NULL;

}

AvahiClient* avahi_service_resolver_get_client (AvahiServiceResolver *r) {
    assert (r);

    return r->client;
}

int avahi_service_resolver_free(AvahiServiceResolver *r) {
    AvahiClient *client;
    int ret = AVAHI_OK;

    assert(r);
    client = r->client;

    if (r->path && client->state != AVAHI_CLIENT_DISCONNECTED)
        ret = avahi_client_simple_method_call(client, r->path, AVAHI_DBUS_INTERFACE_SERVICE_RESOLVER, "Free");

    AVAHI_LLIST_REMOVE(AvahiServiceResolver, service_resolvers, client->service_resolvers, r);

    avahi_free(r->path);
    avahi_free(r);

    return ret;
}

/* AvahiHostNameResolver implementation */

DBusHandlerResult avahi_host_name_resolver_event (AvahiClient *client, AvahiResolverEvent event, DBusMessage *message) {
    AvahiHostNameResolver *r = NULL;
    DBusError error;
    const char *path;

    assert(client);
    assert(message);
    
    dbus_error_init (&error);

    if (!(path = dbus_message_get_path(message)))
        goto fail;

    for (r = client->host_name_resolvers; r; r = r->host_name_resolvers_next)
        if (strcmp (r->path, path) == 0)
            break;

    if (!r)
        goto fail;

    if (event == AVAHI_RESOLVER_FOUND) {
        int32_t interface;
        AvahiProtocol protocol, aprotocol;
        char *name, *address;
        AvahiAddress a;
        
        if (!dbus_message_get_args(
                message, &error,
                DBUS_TYPE_INT32, &interface,
                DBUS_TYPE_INT32, &protocol,
                DBUS_TYPE_STRING, &name,
                DBUS_TYPE_INT32, &aprotocol,
                DBUS_TYPE_STRING, &address,
                DBUS_TYPE_INVALID) ||
            dbus_error_is_set (&error)) {
            fprintf(stderr, "Failed to parse resolver event.\n");
            goto fail;
        }
        
        assert(address);
        if (!avahi_address_parse(address, (AvahiProtocol) aprotocol, &a)) {
            fprintf(stderr, "Failed to parse address\n");
            goto fail;
        }
    
        r->callback(r, (AvahiIfIndex) interface, (AvahiProtocol) protocol, AVAHI_RESOLVER_FOUND, name, &a, r->userdata);

    } else {

        assert(event == AVAHI_RESOLVER_TIMEOUT);

        r->callback(r, (AvahiIfIndex) 0, (AvahiProtocol) 0, AVAHI_RESOLVER_TIMEOUT, NULL, NULL, r->userdata);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
    
fail:
    dbus_error_free (&error);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


AvahiHostNameResolver * avahi_host_name_resolver_new(
    AvahiClient *client,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    const char *name,
    AvahiProtocol aprotocol,
    AvahiHostNameResolverCallback callback,
    void *userdata) {

    DBusError error;
    AvahiHostNameResolver *r;
    DBusMessage *message = NULL, *reply = NULL;
    int32_t i_interface, i_protocol, i_aprotocol;
    char *path;
    
    assert(client);
    assert(name);

    dbus_error_init (&error);

    if (client->state == AVAHI_CLIENT_DISCONNECTED) {
        avahi_client_set_errno(client, AVAHI_ERR_BAD_STATE);
        goto fail;
    }

    if (!(r = avahi_new(AvahiHostNameResolver, 1))) {
        avahi_client_set_errno(client, AVAHI_ERR_NO_MEMORY);
        goto fail;
    }

    r->client = client;
    r->callback = callback;
    r->userdata = userdata;
    r->path = NULL;
    
    AVAHI_LLIST_PREPEND(AvahiHostNameResolver, host_name_resolvers, client->host_name_resolvers, r);

    if (!(message = dbus_message_new_method_call(AVAHI_DBUS_NAME, AVAHI_DBUS_PATH_SERVER, AVAHI_DBUS_INTERFACE_SERVER, "HostNameResolverNew"))) {
        avahi_client_set_errno(client, AVAHI_ERR_NO_MEMORY);
        goto fail;
    }

    i_interface = interface;
    i_protocol = protocol;
    i_aprotocol = aprotocol;

    if (!(dbus_message_append_args(
              message,
              DBUS_TYPE_INT32, &i_interface,
              DBUS_TYPE_INT32, &i_protocol,
              DBUS_TYPE_STRING, &name,
              DBUS_TYPE_INT32, &i_aprotocol,
              DBUS_TYPE_INVALID))) {
        avahi_client_set_errno(client, AVAHI_ERR_NO_MEMORY);
        goto fail;
    }

    if (!(reply = dbus_connection_send_with_reply_and_block(client->bus, message, -1, &error)) ||
        dbus_error_is_set(&error)) {
        avahi_client_set_errno(client, AVAHI_ERR_DBUS_ERROR);
        goto fail;
    }

    if (!dbus_message_get_args (reply, &error, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID) ||
        dbus_error_is_set(&error) ||
        !path) {
        avahi_client_set_errno(client, AVAHI_ERR_DBUS_ERROR);
        goto fail;
    }

    if (!(r->path = avahi_strdup(path))) {

        /* FIXME: We don't remove the object on the server side */

        avahi_client_set_errno(client, AVAHI_ERR_NO_MEMORY);
        goto fail;
    }

    dbus_message_unref(message);
    dbus_message_unref(reply);

    return r;
    
fail:

    if (dbus_error_is_set(&error)) {
        avahi_client_set_dbus_error(client, &error);
        dbus_error_free(&error);
    }

    if (r)
        avahi_host_name_resolver_free(r);
    
    if (message)
        dbus_message_unref(message);

    if (reply)
        dbus_message_unref(reply);

    return NULL;

}

int avahi_host_name_resolver_free(AvahiHostNameResolver *r) {
    int ret = AVAHI_OK;
    AvahiClient *client;

    assert(r);
    client = r->client;

    if (r->path && client->state != AVAHI_CLIENT_DISCONNECTED)
        ret = avahi_client_simple_method_call(client, r->path, AVAHI_DBUS_INTERFACE_HOST_NAME_RESOLVER, "Free");

    AVAHI_LLIST_REMOVE(AvahiHostNameResolver, host_name_resolvers, client->host_name_resolvers, r);

    avahi_free(r->path);
    avahi_free(r);

    return ret;
}

AvahiClient* avahi_host_name_resolver_get_client (AvahiHostNameResolver *r) {
    assert (r);

    return r->client;
}

/* AvahiAddressResolver implementation */

DBusHandlerResult avahi_address_resolver_event (AvahiClient *client, AvahiResolverEvent event, DBusMessage *message) {
    AvahiAddressResolver *r = NULL;
    DBusError error;
    const char *path;

    assert(client);
    assert(message);
    
    dbus_error_init (&error);

    if (!(path = dbus_message_get_path(message)))
        goto fail;

    for (r = client->address_resolvers; r; r = r->address_resolvers_next)
        if (strcmp (r->path, path) == 0)
            break;

    if (!r)
        goto fail;

    if (event == AVAHI_RESOLVER_FOUND) {
        int32_t interface;
        AvahiProtocol protocol, aprotocol;
        char *name, *address;
        AvahiAddress a;
        
        if (!dbus_message_get_args(
                message, &error,
                DBUS_TYPE_INT32, &interface,
                DBUS_TYPE_INT32, &protocol,
                DBUS_TYPE_INT32, &aprotocol,
                DBUS_TYPE_STRING, &address,
                DBUS_TYPE_STRING, &name,
                DBUS_TYPE_INVALID) ||
            dbus_error_is_set (&error)) {
            fprintf(stderr, "Failed to parse resolver event.\n");
            goto fail;
        }
        
        assert(address);
        if (!avahi_address_parse(address, (AvahiProtocol) aprotocol, &a)) {
            fprintf(stderr, "Failed to parse address\n");
            goto fail;
        }
    
        r->callback(r, (AvahiIfIndex) interface, (AvahiProtocol) protocol, AVAHI_RESOLVER_FOUND, (AvahiProtocol) aprotocol, &a, name, r->userdata);
    } else {

        assert(event == AVAHI_RESOLVER_TIMEOUT);

        r->callback(r, (AvahiIfIndex) 0, (AvahiProtocol) 0, AVAHI_RESOLVER_TIMEOUT, (AvahiProtocol) 0, NULL, NULL, r->userdata);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
    
fail:
    dbus_error_free (&error);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

AvahiAddressResolver * avahi_address_resolver_new_a(
    AvahiClient *client,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    const AvahiAddress *a,
    AvahiAddressResolverCallback callback,
    void *userdata) {

    char addr[64];

    assert (a);

    if (!avahi_address_snprint (addr, sizeof (addr), a)) {
        avahi_client_set_errno(client, AVAHI_ERR_INVALID_ADDRESS);
        return NULL;
    }

    return avahi_address_resolver_new (
        client, interface, protocol,
        addr,
        callback, userdata);
}

AvahiAddressResolver * avahi_address_resolver_new(
    AvahiClient *client,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    const char *address,
    AvahiAddressResolverCallback callback,
    void *userdata) {

    DBusError error;
    AvahiAddressResolver *r;
    DBusMessage *message = NULL, *reply = NULL;
    int32_t i_interface;
    AvahiProtocol i_protocol;
    char *path;
    
    assert(client);

    dbus_error_init (&error);

    if (client->state == AVAHI_CLIENT_DISCONNECTED) {
        avahi_client_set_errno(client, AVAHI_ERR_BAD_STATE);
        goto fail;
    }

    if (!(r = avahi_new(AvahiAddressResolver, 1))) {
        avahi_client_set_errno(client, AVAHI_ERR_NO_MEMORY);
        goto fail;
    }

    r->client = client;
    r->callback = callback;
    r->userdata = userdata;
    r->path = NULL;
    
    AVAHI_LLIST_PREPEND(AvahiAddressResolver, address_resolvers, client->address_resolvers, r);

    if (!(message = dbus_message_new_method_call(AVAHI_DBUS_NAME, AVAHI_DBUS_PATH_SERVER, AVAHI_DBUS_INTERFACE_SERVER, "AddressResolverNew"))) {
        avahi_client_set_errno(client, AVAHI_ERR_NO_MEMORY);
        goto fail;
    }

    i_interface = interface;
    i_protocol = protocol;

    if (!(dbus_message_append_args(
              message,
              DBUS_TYPE_INT32, &i_interface,
              DBUS_TYPE_INT32, &i_protocol,
              DBUS_TYPE_STRING, &address,
              DBUS_TYPE_INVALID))) {
        avahi_client_set_errno(client, AVAHI_ERR_NO_MEMORY);
        goto fail;
    }

    if (!(reply = dbus_connection_send_with_reply_and_block(client->bus, message, -1, &error)) ||
        dbus_error_is_set(&error)) {
        avahi_client_set_errno(client, AVAHI_ERR_DBUS_ERROR);
        goto fail;
    }

    if (!dbus_message_get_args (reply, &error, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID) ||
        dbus_error_is_set(&error) ||
        !path) {
        avahi_client_set_errno(client, AVAHI_ERR_DBUS_ERROR);
        goto fail;
    }
    
    if (!(r->path = avahi_strdup(path))) {

        /* FIXME: We don't remove the object on the server side */

        avahi_client_set_errno(client, AVAHI_ERR_NO_MEMORY);
        goto fail;
    }

    dbus_message_unref(message);
    dbus_message_unref(reply);

    return r;
    
fail:

    if (dbus_error_is_set(&error)) {
        avahi_client_set_dbus_error(client, &error);
        dbus_error_free(&error);
    }

    if (r)
        avahi_address_resolver_free(r);
    
    if (message)
        dbus_message_unref(message);

    if (reply)
        dbus_message_unref(reply);

    return NULL;

}

AvahiClient* avahi_address_resolver_get_client (AvahiAddressResolver *r) {
    assert (r);

    return r->client;
}

int avahi_address_resolver_free(AvahiAddressResolver *r) {
    AvahiClient *client;
    int ret = AVAHI_OK;

    assert(r);
    client = r->client;

    if (r->path && client->state != AVAHI_CLIENT_DISCONNECTED)
        ret = avahi_client_simple_method_call(client, r->path, AVAHI_DBUS_INTERFACE_ADDRESS_RESOLVER, "Free");

    AVAHI_LLIST_REMOVE(AvahiAddressResolver, address_resolvers, client->address_resolvers, r);

    avahi_free(r->path);
    avahi_free(r);

    return ret;
}
