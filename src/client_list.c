/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
 \********************************************************************/

/** @file client_list.c
  @brief Client List Functions
  @author Copyright (C) 2004 Alexandre Carmel-Veillex <acv@acv.ca>
  @author Copyright (C) 2007 Paul Kube <nodogsplash@kokoro.ucsd.edu>
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <string.h>

#include "safe.h"
#include "debug.h"
#include "conf.h"
#include "client_list.h"
#include "http_microhttpd.h"
#include "fw_iptables.h"
#include "util.h"


/** Client counter */
static int client_count = 0;
static int client_id = 1;

/** Time last client added */
static unsigned long int last_client_time = 0;

/** Global mutex to protect access to the client list */
pthread_mutex_t client_list_mutex = PTHREAD_MUTEX_INITIALIZER;

/** @internal
 * Holds a pointer to the first element of the list
 */
static t_client *firstclient = NULL;

/** Return current length of the client list
 */
int
get_client_list_length()
{
	return client_count;
}

/** Get the first element of the client list
 */
t_client *
client_get_first_client(void)
{
	return firstclient;
}

/**
 * Initialize the list of connected clients
 */
void
client_list_init(void)
{
	firstclient = NULL;
	client_count = 0;
}

/** @internal
 * Given IP, MAC, and client token, appends a new entry
 * to the end of the client list and returns a pointer to the new entry.
 * All the memory allocation for a list entry is done here.
 * Checks for number of current clients.
 * Does not check for duplicate entries; so check before calling.
 * @param ip IP address
 * @param mac MAC address
 * @param token Token
 * @return Pointer to the client we just created
 */
t_client *
_client_list_append(const char ip[], const char mac[], const char token[])
{
	t_client *client, *prevclient;
	s_config *config;

	config = config_get_config();
	if (client_count >= config->maxclients) {
		debug(LOG_NOTICE, "Already list %d clients, cannot add %s %s", client_count, ip, mac);
		return NULL;
	}

	prevclient = NULL;
	client = firstclient;

	while (client != NULL) {
		prevclient = client;
		client = client->next;
	}

	client = safe_malloc(sizeof(t_client));
	memset(client, 0, sizeof(t_client));

	client->ip = safe_strdup(ip);
	client->mac = safe_strdup(mac);
	client->token = token ? safe_strdup(token) : NULL;
	client->fw_connection_state = FW_MARK_PREAUTHENTICATED;
	client->counters.incoming = 0;
	client->counters.incoming_history = 0;
	client->counters.outgoing = 0;
	client->counters.outgoing_history = 0;
	last_client_time = time(NULL);
	client->counters.last_updated = last_client_time;
	/* Session has not started and not ended yet */
	client->session_start = 0;
	client->session_end = 0;
	client->id = client_id;

	debug(LOG_NOTICE, "Adding %s %s token %s to client list",
		  client->ip, client->mac, client->token ? client->token : "none");

	if (prevclient == NULL) {
		firstclient = client;
	} else {
		prevclient->next = client;
	}

	client_id++;
	client_count++;

	return client;
}

/** @internal
 *  Allocate and return an authentication token.
 *  Caller must free.
 *  We just generate a random string of 8 hex digits,
 *  independent of ip and mac.
 */
char *
_client_list_make_auth_token(const char ip[], const char mac[])
{
	char *token = NULL;

	safe_asprintf(&token,"%04hx%04hx", rand16(), rand16());

	return token;
}

/**
 *  Given an IP address, add a client corresponding to that IP to client list.
 *  Return a pointer to the new client list entry, or to an existing entry
 *  if one with the given IP already exists.
 *  Return NULL if no new client entry can be created.
 */
t_client *
client_list_add_client(const char ip[])
{
	t_client *client;
	char *token;
	char mac[18];

	if (!check_ip_format(ip)) {
		/* Inappropriate format in IP address */
		debug(LOG_NOTICE, "Illegal IP format [%s]", ip);
		return NULL;
	}

	if (arp_get(mac, ip) != 0) {
		/* We could not get their MAC address */
		debug(LOG_NOTICE, "Could not arp MAC address for %s", ip);
		return NULL;
	}

	if ((client = client_list_find(mac, ip)) == NULL) {
		token = _client_list_make_auth_token(ip, mac);  /* get a new token */
		client = _client_list_append(ip, mac, token);
		free(token);
	} else {
		debug(LOG_INFO, "Client %s %s token %s already on client list", ip, mac, client->token);
	}

	return client;
}

/** Finds a client by its token, IP or MAC.
 * A found client is guaranted to be unique.
 * @param ip IP we are looking for in the linked list
 * @param mac MAC we are looking for in the linked list
 * @return Pointer to the client, or NULL if not found
 */
t_client *
client_list_find_by_any(const char mac[], const char ip[], const char token[])
{
	t_client *client = NULL;

	if (!client && token) {
		client = client_list_find_by_token(token);
	}

	if (!client && ip) {
		client = client_list_find_by_ip(ip);
	}

	if (!client && mac) {
		client = client_list_find_by_mac(mac);
	}

	return client;
}

t_client *
client_list_find(const char mac[], const char ip[])
{
	t_client *ptr;

	ptr = firstclient;
	while (ptr) {
		if (!strcmp(ptr->mac, mac) && !strcmp(ptr->ip, ip)) {
			return ptr;
		}
		ptr = ptr->next;
	}

	return NULL;
}

/**
 * Finds a client by its IP address. Returns NULL if
 * the client could not be found.
 * @return Pointer to the client, or NULL if not found
 */
t_client *
client_list_find_by_id(const unsigned id)
{
	t_client *ptr;

	ptr = firstclient;
	while (ptr) {
		if (ptr->id == id) {
			return ptr;
		}
		ptr = ptr->next;
	}

	return NULL;
}

/**
 * Finds a client by its IP address. Returns NULL if
 * the client could not be found.
 * @return Pointer to the client, or NULL if not found
 */
t_client *
client_list_find_by_ip(const char ip[])
{
	t_client *ptr;

	ptr = firstclient;
	while (ptr) {
		if (!strcmp(ptr->ip, ip)) {
			return ptr;
		}
		ptr = ptr->next;
	}

	return NULL;
}

/**
 * Finds a client by its MAC address. Returns NULL if
 * the client could not be found.
 * @return Pointer to the client, or NULL if not found
 */
t_client *
client_list_find_by_mac(const char mac[])
{
	t_client *ptr;

	ptr = firstclient;
	while (ptr) {
		if (!strcmp(ptr->mac, mac)) {
			return ptr;
		}
		ptr = ptr->next;
	}

	return NULL;
}

/**
 * Finds a client by token. Returns NULL if
 * the client could not be found.
 * @return Pointer to the client, or NULL if not found
 */
t_client *
client_list_find_by_token(const char token[])
{
	t_client *ptr;

	ptr = firstclient;
	while (ptr) {
		if (!strcmp(ptr->token, token)) {
			return ptr;
		}
		ptr = ptr->next;
	}

	return NULL;
}

/** @internal
 * @brief Frees the memory used by a t_client structure
 * This function frees the memory used by the t_client structure in the
 * proper order.
 * @param client Points to the client to be freed
 */
static void
_client_list_free_node(t_client *client)
{
	if (client->mac)
		free(client->mac);

	if (client->ip)
		free(client->ip);

	if (client->token)
		free(client->token);

	free(client);
}

/**
 * @brief Deletes a client from the client list
 *
 * Removes the specified client from the client list and then calls
 * the function _client_list_free_node to free the memory taken by the client.
 * @param client Points to the client to be deleted
 */
void
client_list_delete(t_client *client)
{
	t_client *ptr;

	ptr = firstclient;

	if (ptr == NULL) {
		debug(LOG_ERR, "Node list empty!");
	} else if (ptr == client) {
		debug(LOG_NOTICE, "Deleting %s %s token %s from client list",
			  client->ip, client->mac, client->token ? client->token : "none");
		firstclient = ptr->next;
		_client_list_free_node(client);
		client_count--;
	} else {
		/* Loop forward until we reach our point in the list. */
		while (ptr->next != NULL && ptr->next != client) {
			ptr = ptr->next;
		}
		/* If we reach the end before finding out element, complain. */
		if (ptr->next == NULL) {
			debug(LOG_ERR, "Node to delete could not be found.");
		} else {
			/* Free element. */
			debug(LOG_NOTICE, "Deleting %s %s token %s from client list",
				  client->ip, client->mac, client->token ? client->token : "none");
			ptr->next = client->next;
			_client_list_free_node(client);
			client_count--;
		}
	}
}
