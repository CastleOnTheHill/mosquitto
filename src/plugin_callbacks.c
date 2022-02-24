/*
Copyright (c) 2016-2021 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License 2.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   https://www.eclipse.org/legal/epl-2.0/
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

Contributors:
   Roger Light - initial implementation and documentation.
*/

#include "config.h"

#include "mosquitto_broker_internal.h"
#include "memory_mosq.h"
#include "utlist.h"
#include "lib_load.h"


static const char *get_event_name(int event)
{
	switch(event){
		case MOSQ_EVT_RELOAD:
			return "reload";
		case MOSQ_EVT_ACL_CHECK:
			return "acl-check";
		case MOSQ_EVT_BASIC_AUTH:
			return "basic-auth";
		case MOSQ_EVT_PSK_KEY:
			return "psk-key";
		case MOSQ_EVT_EXT_AUTH_START:
			return "auth-start";
		case MOSQ_EVT_EXT_AUTH_CONTINUE:
			return "auth-continue";
		case MOSQ_EVT_MESSAGE:
			return "message";
		case MOSQ_EVT_TICK:
			return "tick";
		case MOSQ_EVT_DISCONNECT:
			return "disconnect";
		case MOSQ_EVT_CONNECT:
			return "connect";
		default:
			return "";
	}
}

static bool check_callback_exists(struct mosquitto__callback *cb_base, MOSQ_FUNC_generic_callback cb_func)
{
	struct mosquitto__callback *tail, *tmp;

	DL_FOREACH_SAFE(cb_base, tail, tmp){
		if(tail->cb == cb_func){
			return true;
		}
	}
	return false;
}

static struct mosquitto__callback **plugin__get_callback_base(struct mosquitto__security_options *security_options, int event)
{
	switch(event){
		case MOSQ_EVT_RELOAD:
			return &security_options->plugin_callbacks.reload;
		case MOSQ_EVT_ACL_CHECK:
			return &security_options->plugin_callbacks.acl_check;
		case MOSQ_EVT_BASIC_AUTH:
			return &security_options->plugin_callbacks.basic_auth;
		case MOSQ_EVT_PSK_KEY:
			return &security_options->plugin_callbacks.psk_key;
		case MOSQ_EVT_EXT_AUTH_START:
			return &security_options->plugin_callbacks.ext_auth_start;
		case MOSQ_EVT_EXT_AUTH_CONTINUE:
			return &security_options->plugin_callbacks.ext_auth_continue;
		case MOSQ_EVT_CONTROL:
			return NULL;
		case MOSQ_EVT_MESSAGE:
			return &security_options->plugin_callbacks.message;
		case MOSQ_EVT_TICK:
			return &security_options->plugin_callbacks.tick;
		case MOSQ_EVT_DISCONNECT:
			return &security_options->plugin_callbacks.disconnect;
		case MOSQ_EVT_CONNECT:
			return &security_options->plugin_callbacks.connect;
		case MOSQ_EVT_PERSIST_RESTORE:
			return &security_options->plugin_callbacks.persist_restore;
		case MOSQ_EVT_PERSIST_CLIENT_ADD:
			return &security_options->plugin_callbacks.persist_client_add;
		case MOSQ_EVT_PERSIST_CLIENT_DELETE:
			return &security_options->plugin_callbacks.persist_client_delete;
		case MOSQ_EVT_PERSIST_CLIENT_UPDATE:
			return &security_options->plugin_callbacks.persist_client_update;
		case MOSQ_EVT_PERSIST_SUBSCRIPTION_ADD:
			return &security_options->plugin_callbacks.persist_subscription_add;
		case MOSQ_EVT_PERSIST_SUBSCRIPTION_DELETE:
			return &security_options->plugin_callbacks.persist_subscription_delete;
		case MOSQ_EVT_PERSIST_CLIENT_MSG_ADD:
			return &security_options->plugin_callbacks.persist_client_msg_add;
		case MOSQ_EVT_PERSIST_CLIENT_MSG_DELETE:
			return &security_options->plugin_callbacks.persist_client_msg_delete;
		case MOSQ_EVT_PERSIST_CLIENT_MSG_UPDATE:
			return &security_options->plugin_callbacks.persist_client_msg_update;
		case MOSQ_EVT_PERSIST_CLIENT_MSG_CLEAR:
			return &security_options->plugin_callbacks.persist_client_msg_clear;
		case MOSQ_EVT_PERSIST_MSG_ADD:
			return &security_options->plugin_callbacks.persist_msg_add;
		case MOSQ_EVT_PERSIST_MSG_DELETE:
			return &security_options->plugin_callbacks.persist_msg_delete;
		case MOSQ_EVT_PERSIST_MSG_LOAD:
			return &security_options->plugin_callbacks.persist_msg_load;
		case MOSQ_EVT_PERSIST_RETAIN_ADD:
			return &security_options->plugin_callbacks.persist_retain_add;
		case MOSQ_EVT_PERSIST_RETAIN_DELETE:
			return &security_options->plugin_callbacks.persist_retain_delete;
		default:
			return NULL;
	}
}



static int remove_callback(mosquitto_plugin_id_t *identifier, int event, struct mosquitto__callback **cb_base, MOSQ_FUNC_generic_callback cb_func)
{
	struct mosquitto__callback *tail, *tmp;
	struct plugin_own_callback *own, *own_tmp;

	DL_FOREACH_SAFE(*cb_base, tail, tmp){
		if(tail->cb == cb_func){
			DL_DELETE(*cb_base, tail);
			mosquitto__FREE(tail);
			break;
		}
	}
	DL_FOREACH_SAFE(identifier->own_callbacks, own, own_tmp){
		if(own->cb_func == cb_func && own->event == event){
			DL_DELETE(identifier->own_callbacks, own);
			mosquitto__FREE(own);
			return MOSQ_ERR_SUCCESS;
		}
	}
	return MOSQ_ERR_NOT_FOUND;
}


int mosquitto_callback_register(
		mosquitto_plugin_id_t *identifier,
		int event,
		MOSQ_FUNC_generic_callback cb_func,
		const void *event_data,
		void *userdata)
{
	struct mosquitto__callback **cb_base = NULL, *cb_new;
	struct mosquitto__security_options *security_options;
	struct plugin_own_callback *own_callback;

	if(cb_func == NULL) return MOSQ_ERR_INVAL;

	if(identifier->listener == NULL){
		security_options = &db.config->security_options;
	}else{
		security_options = &identifier->listener->security_options;
	}

	if(event == MOSQ_EVT_CONTROL){
		return control__register_callback(identifier, cb_func, event_data, userdata);
	}

	cb_base = plugin__get_callback_base(security_options, event);
	if(cb_base == NULL){
		return MOSQ_ERR_NOT_SUPPORTED;
	}

	if(check_callback_exists(*cb_base, cb_func)){
		return MOSQ_ERR_ALREADY_EXISTS;
	}

	cb_new = mosquitto__calloc(1, sizeof(struct mosquitto__callback));
	if(cb_new == NULL){
		return MOSQ_ERR_NOMEM;
	}
	own_callback = mosquitto__calloc(1, sizeof(struct plugin_own_callback));
	if(own_callback == NULL){
		mosquitto__FREE(cb_new);
		return MOSQ_ERR_NOMEM;
	}
	own_callback->event = event;
	own_callback->cb_func = cb_func;
	DL_APPEND(identifier->own_callbacks, own_callback);

	DL_APPEND(*cb_base, cb_new);
	cb_new->cb = cb_func;
	cb_new->userdata = userdata;

	if(identifier->plugin_name){
		log__printf(NULL, MOSQ_LOG_INFO, "Plugin %s has registered to receive '%s' events.",
				identifier->plugin_name, get_event_name(event));
	}

	return MOSQ_ERR_SUCCESS;
}


int plugin__callback_unregister_all(mosquitto_plugin_id_t *identifier)
{
	struct mosquitto__callback **cb_base = NULL;
	struct mosquitto__security_options *security_options;
	struct plugin_own_callback *own, *own_tmp;

	if(identifier == NULL){
		return MOSQ_ERR_INVAL;
	}

	if(identifier->listener == NULL){
		security_options = &db.config->security_options;
	}else{
		security_options = &identifier->listener->security_options;
	}

	control__unregister_all_callbacks(identifier);

	DL_FOREACH_SAFE(identifier->own_callbacks, own, own_tmp){
		cb_base = plugin__get_callback_base(security_options, own->event);
		if(cb_base){
			remove_callback(identifier, own->event, cb_base, own->cb_func);
		}
	}
	return MOSQ_ERR_SUCCESS;
}


int mosquitto_callback_unregister(
		mosquitto_plugin_id_t *identifier,
		int event,
		MOSQ_FUNC_generic_callback cb_func,
		const void *event_data)
{
	struct mosquitto__callback **cb_base = NULL;
	struct mosquitto__security_options *security_options;

	if(identifier == NULL || cb_func == NULL){
		return MOSQ_ERR_INVAL;
	}

	if(identifier->listener == NULL){
		security_options = &db.config->security_options;
	}else{
		security_options = &identifier->listener->security_options;
	}

	if(event == MOSQ_EVT_CONTROL){
		return control__unregister_callback(identifier, cb_func, event_data);
	}

	cb_base = plugin__get_callback_base(security_options, event);
	if(cb_base){
		return remove_callback(identifier, event, cb_base, cb_func);
	}else{
		return MOSQ_ERR_NOT_SUPPORTED;
	}
}