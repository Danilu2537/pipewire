/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <errno.h>

#include <spa/utils/result.h>
#include <spa/param/props.h>

#include <pipewire/pipewire.h>

#include <pulse/context.h>
#include <pulse/timeval.h>
#include <pulse/error.h>

#include "internal.h"

int pa_context_set_error(PA_CONST pa_context *c, int error) {
	pa_assert(error >= 0);
	pa_assert(error < PA_ERR_MAX);
	if (c && c->error != error) {
		pw_log_debug("context %p: error %d %s", c, error, pa_strerror(error));
		((pa_context*)c)->error = error;

	}
	return error;
}

static void global_free(pa_context *c, struct global *g)
{
	spa_list_remove(&g->link);

	if (g->destroy)
		g->destroy(g);
	if (g->proxy) {
		spa_hook_remove(&g->object_listener);
		spa_hook_remove(&g->proxy_listener);
		pw_proxy_destroy(g->proxy);
	}
	if (g->props)
		pw_properties_free(g->props);
	free(g);
}

static void context_unlink(pa_context *c)
{
	pa_stream *s, *t;
	struct global *g;
	pa_operation *o;

	pw_log_debug("context %p: unlink %d", c, c->state);

	c->disconnect = true;
	c->state_callback = NULL;
	c->state_userdata = NULL;

	spa_list_for_each_safe(s, t, &c->streams, link) {
		pa_stream_set_state(s, c->state == PA_CONTEXT_FAILED ?
				PA_STREAM_FAILED : PA_STREAM_TERMINATED);
	}
	spa_list_consume(g, &c->globals, link)
		global_free(c, g);

	spa_list_consume(o, &c->operations, link)
		pa_operation_cancel(o);
}

void pa_context_set_state(pa_context *c, pa_context_state_t st) {
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	if (c->state == st)
		return;

	pw_log_debug("context %p: state %d", c, st);

	pa_context_ref(c);

	c->state = st;

	if (c->state_callback)
		c->state_callback(c, c->state_userdata);

	if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED)
		context_unlink(c);

	pa_context_unref(c);
}

static void context_fail(pa_context *c, int error) {
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	pw_log_debug("context %p: error %d", c, error);

	pa_context_set_error(c, error);
	pa_context_set_state(c, PA_CONTEXT_FAILED);
}

SPA_EXPORT
pa_context *pa_context_new(pa_mainloop_api *mainloop, const char *name)
{
	return pa_context_new_with_proplist(mainloop, name, NULL);
}

struct global *pa_context_find_global(pa_context *c, uint32_t id)
{
	struct global *g;
	spa_list_for_each(g, &c->globals, link) {
		if (g->id == id)
			return g;
	}
	return NULL;
}

struct global *pa_context_find_global_by_name(pa_context *c, uint32_t mask, const char *name)
{
	struct global *g;
	const char *str;
	uint32_t id = atoi(name);

	spa_list_for_each(g, &c->globals, link) {
		if ((g->mask & mask) == 0)
			continue;
		if (g->props != NULL &&
		    (str = pw_properties_get(g->props, PW_KEY_NODE_NAME)) != NULL &&
		    strcmp(str, name) == 0)
			return g;
		if (g->id == id)
			return g;
	}
	return NULL;
}

struct global *pa_context_find_linked(pa_context *c, uint32_t idx)
{
	struct global *g, *f;

	spa_list_for_each(g, &c->globals, link) {
		if (g->type != PW_TYPE_INTERFACE_EndpointLink)
			continue;

		pw_log_debug("context %p: %p %d %d:%d %d:%d", c, g, idx,
				g->link_info.output->id, g->link_info.output->endpoint_info.node_id,
				g->link_info.input->id, g->link_info.input->endpoint_info.node_id);

		if (g->link_info.input->id == idx ||
		    g->link_info.input->endpoint_info.node_id == idx)
			f = g->link_info.output;
		else if (g->link_info.output->id == idx ||
		    g->link_info.output->endpoint_info.node_id == idx)
			f = g->link_info.input;
		else
			continue;

		if (f == NULL || ((f->mask & (PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SINK)) == 0))
			continue;

		return f;
	}
	return NULL;
}

static void emit_event(pa_context *c, struct global *g, pa_subscription_event_type_t event)
{
	if (c->subscribe_callback && (c->subscribe_mask & g->mask)) {
		pw_log_debug("context %p: obj %d: emit %d:%d", c, g->id, event, g->event);
		c->subscribe_callback(c,
				event | g->event,
				g->id,
				c->subscribe_userdata);
	}
}

static void update_device_props(struct global *g)
{
	pa_card_info *i = &g->card_info.info;
	const char *s;

	if ((s = pa_proplist_gets(i->proplist, PW_KEY_DEVICE_ICON_NAME)))
		pa_proplist_sets(i->proplist, PA_PROP_DEVICE_ICON_NAME, s);
}

static void device_event_info(void *object, const struct pw_device_info *update)
{
        struct global *g = object;
	pa_card_info *i = &g->card_info.info;
	const char *str;
	uint32_t n;
	struct pw_device_info *info;

	pw_log_debug("global %p: id:%d change-mask:%08lx", g, g->id, update->change_mask);
	info = g->info = pw_device_info_update(g->info, update);

	i->index = g->id;
	i->name = info->props ?
		spa_dict_lookup(info->props, PW_KEY_DEVICE_NAME) : "unknown";
	str = info->props ? spa_dict_lookup(info->props, PW_KEY_MODULE_ID) : NULL;
	i->owner_module = str ? (unsigned)atoi(str) : SPA_ID_INVALID;
	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS) {
		i->driver = info->props ?
			spa_dict_lookup(info->props, PW_KEY_DEVICE_API) : NULL;

		if (i->proplist)
			pa_proplist_update_dict(i->proplist, info->props);
		else {
			i->proplist = pa_proplist_new_dict(info->props);
		}
		update_device_props(g);
	}
	info->change_mask = update->change_mask;
	if (update->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) {
		for (n = 0; n < info->n_params; n++) {
			if (!(info->params[n].flags & SPA_PARAM_INFO_READ))
				continue;

			switch (info->params[n].id) {
			case SPA_PARAM_EnumProfile:
				pw_device_proxy_enum_params((struct pw_device_proxy*)g->proxy,
					0, SPA_PARAM_EnumProfile, 0, -1, NULL);
				break;
			case SPA_PARAM_Profile:
				pw_device_proxy_enum_params((struct pw_device_proxy*)g->proxy,
					0, SPA_PARAM_Profile, 0, -1, NULL);
				break;
			default:
				break;
			}
		}
	}
	g->pending_seq = pw_proxy_sync(g->proxy, 0);
}

static void device_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct global *g = object;

	switch (id) {
	case SPA_PARAM_EnumProfile:
	{
		uint32_t id;
		const char *name;
		struct param *p;

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&id),
				SPA_PARAM_PROFILE_name,  SPA_POD_String(&name)) < 0) {
			pw_log_warn("device %d: can't parse profile", g->id);
			return;
		}
		p = malloc(sizeof(struct param) + SPA_POD_SIZE(param));
		if (p) {
			p->id = id;
			p->seq = seq;
			p->param = SPA_MEMBER(p, sizeof(struct param), struct spa_pod);
			memcpy(p->param, param, SPA_POD_SIZE(param));
			spa_list_append(&g->card_info.profiles, &p->link);
			g->card_info.n_profiles++;
		}
		pw_log_debug("device %d: enum profile %d: \"%s\"", g->id, id, name);
		break;
	}
	case SPA_PARAM_Profile:
	{
		uint32_t id;
		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&id)) < 0) {
			pw_log_warn("device %d: can't parse profile", g->id);
			return;
		}
		g->card_info.active_profile = id;
		pw_log_debug("device %d: current profile %d", g->id, id);
		break;
	}
	default:
		break;
	}
}

static const struct pw_device_proxy_events device_events = {
	PW_VERSION_ENDPOINT_PROXY_EVENTS,
	.info = device_event_info,
	.param = device_event_param,
};

static void device_destroy(void *data)
{
	struct global *global = data;
	struct param *p;

	if (global->card_info.info.proplist)
		pa_proplist_free(global->card_info.info.proplist);
	spa_list_consume(p, &global->card_info.profiles, link) {
		spa_list_remove(&p->link);
		free(p);
	}
	if (global->info)
		pw_device_info_free(global->info);
}

static void endpoint_event_info(void *object, const struct pw_endpoint_info *update)
{
	struct global *g = object;
	struct pw_endpoint_info *info = g->info;
	uint32_t i;

	pw_log_debug("update %d %08x", g->id, update->change_mask);
	if (info == NULL) {
		info = g->info = calloc(1, sizeof(*info));
		info->id = update->id;
		info->name = update->name ? strdup(update->name) : NULL;
		info->media_class = update->media_class ? strdup(update->media_class) : NULL;
		info->direction = update->direction;
		info->flags = update->flags;
	}
	info->change_mask = update->change_mask;

	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_STREAMS)
		info->n_streams = update->n_streams;
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_SESSION)
		info->session_id = update->session_id;
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PARAMS && !g->subscribed) {
		uint32_t subscribed[32], n_subscribed = 0;

		info->n_params = update->n_params;
		free(info->params);
		info->params = malloc(info->n_params * sizeof(struct spa_param_info));
		memcpy(info->params, update->params,
			info->n_params * sizeof(struct spa_param_info));

		for (i = 0; i < info->n_params; i++) {
			switch (info->params[i].id) {
			case SPA_PARAM_EnumRoute:
			case SPA_PARAM_Props:
				subscribed[n_subscribed++] = info->params[i].id;
				break;
			default:
				break;
			}
		}
		if (n_subscribed > 0) {
			pw_endpoint_proxy_subscribe_params((struct pw_endpoint_proxy*)g->proxy,
					subscribed, n_subscribed);
			g->subscribed = true;
		}
	}
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PROPS) {
		if (info->props)
			pw_properties_free ((struct pw_properties *)info->props);
		info->props =
			(struct spa_dict *) pw_properties_new_dict (update->props);

#if 0
		i->name = info->props ?
			spa_dict_lookup(info->props, PW_KEY_ENDPOINT_NAME) : "unknown";
		str = info->props ? spa_dict_lookup(info->props, PW_KEY_MODULE_ID) : NULL;
		i->owner_module = str ? (unsigned)atoi(str) : SPA_ID_INVALID;
		i->driver = info->props ?
			spa_dict_lookup(info->props, PW_KEY_DEVICE_API) : "unknown";

		if (i->proplist)
			pa_proplist_update_dict(i->proplist, info->props);
		else {
			i->proplist = pa_proplist_new_dict(info->props);
		}
#endif
	}
	g->pending_seq = pw_proxy_sync(g->proxy, 0);
}

static void parse_props(struct global *g, const struct spa_pod *param)
{
	struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) param;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
			spa_pod_get_float(&prop->value, &g->endpoint_info.volume);
			break;
		case SPA_PROP_mute:
			spa_pod_get_bool(&prop->value, &g->endpoint_info.mute);
			break;
		case SPA_PROP_channelVolumes:
		{
			uint32_t n_vals;

			n_vals = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					g->endpoint_info.channel_volumes, SPA_AUDIO_MAX_CHANNELS);

			if (n_vals != g->endpoint_info.n_channel_volumes) {
				emit_event(g->context, g, PA_SUBSCRIPTION_EVENT_REMOVE);
				emit_event(g->context, g, PA_SUBSCRIPTION_EVENT_NEW);
				g->endpoint_info.n_channel_volumes = n_vals;
			}
			break;
		}
		default:
			break;
		}
	}
}

/* routing information on the endpoint is mapped to sink/source ports. */
static void parse_route(struct global *g, const struct spa_pod *param)
{
}

static void endpoint_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct global *g = object;
	pw_log_debug("update param %d %d", g->id, id);

	switch (id) {
	case SPA_PARAM_Props:
		parse_props(g, param);
		break;
	case SPA_PARAM_EnumRoute:
		parse_route(g, param);
		break;
	default:
		break;
	}
}

static const struct pw_endpoint_proxy_events endpoint_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = endpoint_event_info,
	.param = endpoint_event_param,
};

static void endpoint_destroy(void *data)
{
	struct global *global = data;
	if (global->info) {
		struct pw_endpoint_info *info = global->info;
		free(info->name);
		free(info->params);
		if (info->props)
			pw_properties_free ((struct pw_properties *)info->props);
		free(info);
	}
}

static void module_event_info(void *object, const struct pw_module_info *info)
{
        struct global *g = object;
	pa_module_info *i = &g->module_info.info;

	pw_log_debug("update %d", g->id);

        info = g->info = pw_module_info_update(g->info, info);

	i->index = g->id;
	if (info->change_mask & PW_MODULE_CHANGE_MASK_PROPS) {
		if (i->proplist)
			pa_proplist_update_dict(i->proplist, info->props);
		else
			i->proplist = pa_proplist_new_dict(info->props);
	}

	i->name = info->name;
	i->argument = info->args;
	i->n_used = -1;
	i->auto_unload = false;
	g->pending_seq = pw_proxy_sync(g->proxy, 0);
}

static const struct pw_module_proxy_events module_events = {
	PW_VERSION_MODULE_PROXY_EVENTS,
	.info = module_event_info,
};

static void module_destroy(void *data)
{
	struct global *global = data;
	if (global->module_info.info.proplist)
		pa_proplist_free(global->module_info.info.proplist);
	if (global->info)
		pw_module_info_free(global->info);
}

static void client_event_info(void *object, const struct pw_client_info *info)
{
        struct global *g = object;
	const char *str;
	pa_client_info *i = &g->client_info.info;

	pw_log_debug("update %d", g->id);
	info = g->info = pw_client_info_update(g->info, info);

	i->index = g->id;
	str = info->props ? spa_dict_lookup(info->props, PW_KEY_MODULE_ID) : NULL;
	i->owner_module = str ? (unsigned)atoi(str) : SPA_ID_INVALID;

	if (info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS) {
		if (i->proplist)
			pa_proplist_update_dict(i->proplist, info->props);
		else
			i->proplist = pa_proplist_new_dict(info->props);
		i->name = info->props ?
			spa_dict_lookup(info->props, PW_KEY_APP_NAME) : NULL;
		i->driver = info->props ?
			spa_dict_lookup(info->props, PW_KEY_PROTOCOL) : NULL;
	}
	g->pending_seq = pw_proxy_sync(g->proxy, 0);
}

static const struct pw_client_proxy_events client_events = {
	PW_VERSION_CLIENT_PROXY_EVENTS,
	.info = client_event_info,
};

static void client_destroy(void *data)
{
	struct global *global = data;
	if (global->client_info.info.proplist)
		pa_proplist_free(global->client_info.info.proplist);
	if (global->info)
		pw_client_info_free(global->info);
}

static void proxy_destroy(void *data)
{
	struct global *g = data;
	spa_hook_remove(&g->proxy_listener);
	g->proxy = NULL;
}

static void proxy_done(void *data, int seq)
{
	struct global *g = data;
	pa_subscription_event_type_t event;

	if (g->pending_seq == seq) {
		if (g->init) {
			g->init = false;
			event = PA_SUBSCRIPTION_EVENT_NEW;
		} else {
			event = PA_SUBSCRIPTION_EVENT_CHANGE;
		}
		emit_event(g->context, g, event);
	}
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = proxy_destroy,
	.done = proxy_done,
};

static int set_mask(pa_context *c, struct global *g)
{
	const char *str;
	const void *events = NULL;
	pw_destroy_t destroy;
	uint32_t client_version;

	switch (g->type) {
	case PW_TYPE_INTERFACE_Device:
		if (g->props == NULL)
			return 0;
		if ((str = pw_properties_get(g->props, PW_KEY_MEDIA_CLASS)) == NULL)
			return 0;
		if (strcmp(str, "Audio/Device") != 0)
			return 0;

		/* devices are turned into card objects */
		pw_log_debug("found card %d", g->id);
		g->mask = PA_SUBSCRIPTION_MASK_CARD;
		g->event = PA_SUBSCRIPTION_EVENT_CARD;

		events = &device_events;
                client_version = PW_VERSION_DEVICE_PROXY;
                destroy = device_destroy;
                spa_list_init(&g->card_info.profiles);
		break;

	case PW_TYPE_INTERFACE_Endpoint:
		if (g->props == NULL)
			return 0;

		if ((str = pw_properties_get(g->props, PW_KEY_PRIORITY_SESSION)) != NULL)
			g->priority_session = pw_properties_parse_int(str);

		if ((str = pw_properties_get(g->props, PW_KEY_MEDIA_CLASS)) == NULL) {
			pw_log_warn("endpoint %d without "PW_KEY_MEDIA_CLASS, g->id);
			return 0;
		}
		g->endpoint_info.monitor = SPA_ID_INVALID;

		/* endpoints get transformed into sink/source or sink_input/source_output */
		if (strcmp(str, "Audio/Sink") == 0) {
			pw_log_debug("found sink %d", g->id);
			g->mask = PA_SUBSCRIPTION_MASK_SINK;
			g->event = PA_SUBSCRIPTION_EVENT_SINK;
		}
		else if (strcmp(str, "Audio/Source") == 0) {
			pw_log_debug("found source %d", g->id);
			g->mask = PA_SUBSCRIPTION_MASK_SOURCE;
			g->event = PA_SUBSCRIPTION_EVENT_SOURCE;
			if ((str = pw_properties_get(g->props, PW_KEY_ENDPOINT_MONITOR)) != NULL) {
				struct global *f;
				f = pa_context_find_global(c, pw_properties_parse_int(str));
				if (f != NULL) {
					g->endpoint_info.monitor = f->id;
					f->endpoint_info.monitor = g->id;
				}
			}
		}
		else if (strcmp(str, "Stream/Output/Audio") == 0) {
			pw_log_debug("found sink input %d", g->id);
			g->mask = PA_SUBSCRIPTION_MASK_SINK_INPUT;
			g->event = PA_SUBSCRIPTION_EVENT_SINK_INPUT;
		}
		else if (strcmp(str, "Stream/Input/Audio") == 0) {
			pw_log_debug("found source output %d", g->id);
			g->mask = PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT;
			g->event = PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT;
		}

		if ((str = pw_properties_get(g->props, PW_KEY_ENDPOINT_CLIENT_ID)) != NULL)
			g->endpoint_info.client_id = atoi(str);
		else if ((str = pw_properties_get(g->props, PW_KEY_CLIENT_ID)) != NULL)
			g->endpoint_info.client_id = atoi(str);
		if ((str = pw_properties_get(g->props, PW_KEY_DEVICE_ID)) != NULL)
			g->endpoint_info.device_id = atoi(str);
		if ((str = pw_properties_get(g->props, PW_KEY_NODE_ID)) != NULL) {
			pa_stream *s;
			g->endpoint_info.node_id = atoi(str);
			spa_list_for_each(s, &c->streams, link) {
				if (pw_stream_get_node_id(s->stream) == g->endpoint_info.node_id)
					s->endpoint_id = g->id;
			}
		}

		events = &endpoint_events;
                client_version = PW_VERSION_ENDPOINT_PROXY;
                destroy = endpoint_destroy;
		g->endpoint_info.volume = 1.0;
		g->endpoint_info.mute = false;
		break;

	case PW_TYPE_INTERFACE_EndpointStream:
		if (g->props == NULL)
			return 0;

		if ((str = pw_properties_get(g->props, PW_KEY_ENDPOINT_ID)) == NULL) {
			pw_log_warn("endpoint stream %d without "PW_KEY_ENDPOINT_ID, g->id);
			return 0;
		}
		/* streams get transformed into profiles on the device */
		pw_log_debug("found endpoint stream %d", g->id);
		break;

	case PW_TYPE_INTERFACE_Module:
		pw_log_debug("found module %d", g->id);
		g->mask = PA_SUBSCRIPTION_MASK_MODULE;
		g->event = PA_SUBSCRIPTION_EVENT_MODULE;
		events = &module_events;
                client_version = PW_VERSION_MODULE_PROXY;
                destroy = module_destroy;
		break;

	case PW_TYPE_INTERFACE_Client:
		pw_log_debug("found client %d", g->id);
		g->mask = PA_SUBSCRIPTION_MASK_CLIENT;
		g->event = PA_SUBSCRIPTION_EVENT_CLIENT;
		events = &client_events;
                client_version = PW_VERSION_CLIENT_PROXY;
                destroy = client_destroy;
		break;

	case PW_TYPE_INTERFACE_EndpointLink:
		if ((str = pw_properties_get(g->props, PW_KEY_ENDPOINT_LINK_OUTPUT_ENDPOINT)) == NULL)
			return 0;
		g->link_info.output = pa_context_find_global(c, pw_properties_parse_int(str));
		if ((str = pw_properties_get(g->props, PW_KEY_ENDPOINT_LINK_INPUT_ENDPOINT)) == NULL)
			return 0;
		g->link_info.input = pa_context_find_global(c, pw_properties_parse_int(str));

		if (g->link_info.output == NULL || g->link_info.input == NULL)
			return 0;

		pw_log_debug("link %d->%d", g->link_info.output->id, g->link_info.input->id);

		if (!g->link_info.output->init)
			emit_event(c, g->link_info.output, PA_SUBSCRIPTION_EVENT_CHANGE);
		if (!g->link_info.input->init)
			emit_event(c, g->link_info.input, PA_SUBSCRIPTION_EVENT_CHANGE);

		break;

	default:
		return 0;
	}

	pw_log_debug("global %p: id:%u mask %d/%d", g, g->id, g->mask, g->event);

	if (events) {
		pw_log_debug("bind %d", g->id);

		g->proxy = pw_registry_proxy_bind(c->registry_proxy, g->id, g->type,
	                                      client_version, 0);
		if (g->proxy == NULL)
	                return -ENOMEM;

		pw_proxy_add_object_listener(g->proxy, &g->object_listener, events, g);
		pw_proxy_add_listener(g->proxy, &g->proxy_listener, &proxy_events, g);
		g->destroy = destroy;
	} else {
		emit_event(c, g, PA_SUBSCRIPTION_EVENT_NEW);
	}

	return 1;
}

static inline void insert_global(pa_context *c, struct global *global)
{
	struct global *g, *t;

	spa_list_for_each_safe(g, t, &c->globals, link) {
		if (g->priority_session <= global->priority_session) {
			break;
		}
	}
	spa_list_append(&g->link, &global->link);
}

static void registry_event_global(void *data, uint32_t id,
                                  uint32_t permissions, uint32_t type, uint32_t version,
                                  const struct spa_dict *props)
{
	pa_context *c = data;
	struct global *g;
	int res;

	g = calloc(1, sizeof(struct global));
	pw_log_debug("context %p: global %d %u %p", c, id, type, g);
	g->context = c;
	g->id = id;
	g->type = type;
	g->init = true;
	g->props = props ? pw_properties_new_dict(props) : NULL;

	res = set_mask(c, g);
	insert_global(c, g);

	if (res != 1)
		global_free(c, g);
}

static void registry_event_global_remove(void *object, uint32_t id)
{
	pa_context *c = object;
	struct global *g;

	pw_log_debug("context %p: remove %d", c, id);
	if ((g = pa_context_find_global(c, id)) == NULL)
		return;

	emit_event(c, g, PA_SUBSCRIPTION_EVENT_REMOVE);

	pw_log_debug("context %p: free %d %p", c, id, g);
	global_free(c, g);
}

static const struct pw_registry_proxy_events registry_events =
{
	PW_VERSION_REGISTRY_PROXY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void complete_operations(pa_context *c, int seq)
{
	pa_operation *o, *t;
	spa_list_for_each_safe(o, t, &c->operations, link) {
		if (o->seq != seq)
			continue;
		pa_operation_ref(o);
		if (o->callback)
			o->callback(o, o->userdata);
		pa_operation_unref(o);
	}
}

static void core_info(void *data, const struct pw_core_info *info)
{
	pa_context *c = data;
	bool first = c->core_info == NULL;

	pw_log_debug("context %p: info", c);

	if (first) {
		pa_context_set_state(c, PA_CONTEXT_AUTHORIZING);
		pa_context_set_state(c, PA_CONTEXT_SETTING_NAME);
	}

	c->core_info = pw_core_info_update(c->core_info, info);

	if (first)
		pa_context_set_state(c, PA_CONTEXT_READY);
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	pa_context *c = data;

	pw_log_error("context %p: error id:%u seq:%d res:%d (%s): %s", c,
			id, seq, res, spa_strerror(res), message);

	if (id == 0) {
		if (!c->disconnect)
			context_fail(c, PA_ERR_CONNECTIONTERMINATED);
	}
}

static void core_done(void *data, uint32_t id, int seq)
{
	pa_context *c = data;
	pw_log_debug("done %d", seq);
	complete_operations(c, seq);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = core_info,
	.done = core_done,
	.error = core_error
};

struct success_data {
	pa_context_success_cb_t cb;
	void *userdata;
	int ret;
};

static void on_success(pa_operation *o, void *userdata)
{
	struct success_data *d = userdata;
	pa_context *c = o->context;
	pa_operation_done(o);
	if (d->cb)
		d->cb(c, d->ret, d->userdata);
}

SPA_EXPORT
pa_operation* pa_context_subscribe(pa_context *c, pa_subscription_mask_t m, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	c->subscribe_mask = m;

	if (c->registry_proxy == NULL) {
		c->registry_proxy = pw_core_get_registry(c->core,
				PW_VERSION_REGISTRY_PROXY, 0);
		pw_registry_proxy_add_listener(c->registry_proxy,
				&c->registry_listener,
				&registry_events, c);
	}

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->ret = 0;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_context *pa_context_new_with_proplist(pa_mainloop_api *mainloop, const char *name, PA_CONST pa_proplist *p)
{
	struct pw_context *context;
	struct pw_loop *loop;
	struct pw_properties *props;
	pa_context *c;

	pa_assert(mainloop);

	props = pw_properties_new(NULL, NULL);
	if (name)
		pw_properties_set(props, PA_PROP_APPLICATION_NAME, name);
	pw_properties_set(props, PW_KEY_CLIENT_API, "pulseaudio");
	if (p)
		pw_properties_update(props, &p->props->dict);

	loop = mainloop->userdata;
	context = pw_context_new(loop, NULL, sizeof(struct pa_context));
	if (context == NULL)
		return NULL;

	c = pw_context_get_user_data(context);
	c->props = props;
	c->loop = loop;
	c->context = context;
	c->proplist = p ? pa_proplist_copy(p) : pa_proplist_new();
	c->refcount = 1;
	c->client_index = PA_INVALID_INDEX;

	if (name)
		pa_proplist_sets(c->proplist, PA_PROP_APPLICATION_NAME, name);

	c->mainloop = mainloop;
	c->error = 0;
	c->state = PA_CONTEXT_UNCONNECTED;

	spa_list_init(&c->globals);

	spa_list_init(&c->streams);
	spa_list_init(&c->operations);

	return c;
}

static void do_context_destroy(pa_mainloop_api*m, void *userdata)
{
	pa_context *c = userdata;
	pw_context_destroy(c->context);
}

static void context_free(pa_context *c)
{
	pw_log_debug("context %p: free", c);

	context_unlink(c);

	pw_properties_free(c->props);
	if (c->proplist)
		pa_proplist_free(c->proplist);
	if (c->core_info)
		pw_core_info_free(c->core_info);

	pa_mainloop_api_once(c->mainloop, do_context_destroy, c);
}

SPA_EXPORT
void pa_context_unref(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	if (--c->refcount == 0)
		context_free(c);
}

SPA_EXPORT
pa_context* pa_context_ref(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);
	c->refcount++;
	return c;
}

SPA_EXPORT
void pa_context_set_state_callback(pa_context *c, pa_context_notify_cb_t cb, void *userdata)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	if (c->state == PA_CONTEXT_TERMINATED || c->state == PA_CONTEXT_FAILED)
		return;

	c->state_callback = cb;
	c->state_userdata = userdata;
}

SPA_EXPORT
void pa_context_set_event_callback(pa_context *c, pa_context_event_cb_t cb, void *userdata)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	if (c->state == PA_CONTEXT_TERMINATED || c->state == PA_CONTEXT_FAILED)
		return;

	c->event_callback = cb;
	c->event_userdata = userdata;
}

SPA_EXPORT
int pa_context_errno(PA_CONST pa_context *c)
{
	if (!c)
		return PA_ERR_INVALID;

	pa_assert(c->refcount >= 1);

	return c->error;
}

SPA_EXPORT
int pa_context_is_pending(PA_CONST pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY(c, PA_CONTEXT_IS_GOOD(c->state), PA_ERR_BADSTATE);

	return !spa_list_is_empty(&c->operations);
}

SPA_EXPORT
pa_context_state_t pa_context_get_state(PA_CONST pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);
	return c->state;
}

SPA_EXPORT
int pa_context_connect(pa_context *c, const char *server, pa_context_flags_t flags, const pa_spawn_api *api)
{
	int res = 0;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY(c, c->state == PA_CONTEXT_UNCONNECTED, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(c, !(flags & ~(PA_CONTEXT_NOAUTOSPAWN|PA_CONTEXT_NOFAIL)), PA_ERR_INVALID);
	PA_CHECK_VALIDITY(c, !server || *server, PA_ERR_INVALID);

	pa_context_ref(c);

	c->no_fail = !!(flags & PA_CONTEXT_NOFAIL);

	pa_context_set_state(c, PA_CONTEXT_CONNECTING);

	c->core = pw_context_connect(c->context, pw_properties_copy(c->props), 0);
	if (c->core == NULL) {
                context_fail(c, PA_ERR_CONNECTIONREFUSED);
		res = -1;
		goto exit;
	}
	pw_core_add_listener(c->core, &c->core_listener, &core_events, c);

exit:
	pa_context_unref(c);

	return res;
}

SPA_EXPORT
void pa_context_disconnect(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	c->disconnect = true;
	if (c->core) {
		pw_core_disconnect(c->core);
		c->core = NULL;
	}
	if (PA_CONTEXT_IS_GOOD(c->state))
		pa_context_set_state(c, PA_CONTEXT_TERMINATED);
}

struct notify_data {
	pa_context_notify_cb_t cb;
	void *userdata;
};

static void on_notify(pa_operation *o, void *userdata)
{
	struct notify_data *d = userdata;
	pa_context *c = o->context;
	pa_operation_done(o);
	if (d->cb)
		d->cb(c, d->userdata);
}

SPA_EXPORT
pa_operation* pa_context_drain(pa_context *c, pa_context_notify_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct notify_data *d;

	o = pa_operation_new(c, NULL, on_notify, sizeof(struct notify_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_exit_daemon(pa_context *c, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_data *d;

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->ret = PA_ERR_ACCESS;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	pw_log_warn("Not Implemented");

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_default_sink(pa_context *c, const char *name, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_data *d;

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->ret = PA_ERR_ACCESS;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	pw_log_warn("Not Implemented");

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_default_source(pa_context *c, const char *name, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_data *d;

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->ret = PA_ERR_ACCESS;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	pw_log_warn("Not Implemented");

	return o;
}

SPA_EXPORT
int pa_context_is_local(PA_CONST pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(c, PA_CONTEXT_IS_GOOD(c->state), PA_ERR_BADSTATE, -1);

	return 1;
}

SPA_EXPORT
pa_operation* pa_context_set_name(pa_context *c, const char *name, pa_context_success_cb_t cb, void *userdata)
{
	struct spa_dict dict;
	struct spa_dict_item items[1];
	pa_operation *o;
	struct success_data *d;
	int changed;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(name);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	items[0] = SPA_DICT_ITEM_INIT(PA_PROP_APPLICATION_NAME, name);
	dict = SPA_DICT_INIT(items, 1);
	changed = pw_properties_update(c->props, &dict);

	if (changed) {
		struct pw_client_proxy *client_proxy;

		client_proxy = pw_core_get_client_proxy(c->core);
		pw_client_proxy_update_properties(client_proxy, &c->props->dict);
	}

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
const char* pa_context_get_server(PA_CONST pa_context *c)
{
	const struct pw_core_info *info;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	info = c->core_info;
	PA_CHECK_VALIDITY_RETURN_NULL(c, info && info->name, PA_ERR_NOENTITY);

	return info->name;
}

SPA_EXPORT
uint32_t pa_context_get_protocol_version(PA_CONST pa_context *c)
{
	return PA_PROTOCOL_VERSION;
}

SPA_EXPORT
uint32_t pa_context_get_server_protocol_version(PA_CONST pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(c, PA_CONTEXT_IS_GOOD(c->state), PA_ERR_BADSTATE, PA_INVALID_INDEX);

	return PA_PROTOCOL_VERSION;
}

SPA_EXPORT
pa_operation *pa_context_proplist_update(pa_context *c, pa_update_mode_t mode, PA_CONST pa_proplist *p, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_data *d;

	spa_assert(c);
	spa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, mode == PA_UPDATE_SET ||
			mode == PA_UPDATE_MERGE || mode == PA_UPDATE_REPLACE, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pa_proplist_update(c->proplist, mode, p);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation *pa_context_proplist_remove(pa_context *c, const char *const keys[], pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_data *d;

	spa_assert(c);
	spa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, keys && keys[0], PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pw_log_warn("Not Implemented");

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
uint32_t pa_context_get_index(PA_CONST pa_context *c)
{
	return c->client_index;
}

SPA_EXPORT
pa_time_event* pa_context_rttime_new(PA_CONST pa_context *c, pa_usec_t usec, pa_time_event_cb_t cb, void *userdata)
{
	struct timeval tv;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(c->mainloop);

	if (usec == PA_USEC_INVALID)
		return c->mainloop->time_new(c->mainloop, NULL, cb, userdata);

	pa_timeval_store(&tv, usec);

	return c->mainloop->time_new(c->mainloop, &tv, cb, userdata);
}

SPA_EXPORT
void pa_context_rttime_restart(PA_CONST pa_context *c, pa_time_event *e, pa_usec_t usec)
{
	struct timeval tv;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(c->mainloop);

	if (usec == PA_USEC_INVALID)
		c->mainloop->time_restart(e, NULL);
	else {
		pa_timeval_store(&tv, usec);
		c->mainloop->time_restart(e, &tv);
	}
}

SPA_EXPORT
size_t pa_context_get_tile_size(PA_CONST pa_context *c, const pa_sample_spec *ss)
{
	size_t fs, mbs;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(c, !ss || pa_sample_spec_valid(ss), PA_ERR_INVALID, (size_t) -1);

	fs = ss ? pa_frame_size(ss) : 1;
	mbs = PA_ROUND_DOWN(4096, fs);
	return PA_MAX(mbs, fs);
}

SPA_EXPORT
int pa_context_load_cookie_from_file(pa_context *c, const char *cookie_file_path)
{
	return 0;
}
