/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/filter.h>

#include <pipewire/pipewire.h>
#include "pipewire/private.h"

#include "module-audio-dsp/audio-dsp.h"

static const struct spa_dict_item module_props[] = {
	{ PW_MODULE_PROP_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_MODULE_PROP_DESCRIPTION, "Manage audio DSP nodes" },
	{ PW_MODULE_PROP_VERSION, PACKAGE_VERSION },
};

struct factory_data {
	struct pw_factory *this;
	struct pw_properties *properties;

	struct spa_list node_list;

	struct pw_module *module;
	struct spa_hook module_listener;
};

struct node_data {
	struct factory_data *data;
	struct spa_list link;
	struct pw_node *dsp;
	struct spa_hook dsp_listener;
	struct spa_hook resource_listener;
};

static void resource_destroy(void *data)
{
	struct node_data *nd = data;
	spa_hook_remove(&nd->resource_listener);
	if (nd->dsp)
		pw_node_destroy(nd->dsp);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = resource_destroy
};

static void node_destroy(void *data)
{
	struct node_data *nd = data;
	spa_list_remove(&nd->link);
	nd->dsp = NULL;
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_destroy
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   uint32_t type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *d = _data;
	struct pw_client *client;
	struct pw_node *dsp;
	int res, maxbuffer;
	const char *str;
	enum pw_direction direction;
	struct node_data *nd;
	struct pw_resource *bound_resource;

	if (resource == NULL)
		goto no_resource;

	client = pw_resource_get_client(resource);

	if ((str = pw_properties_get(properties, "audio-dsp.direction")) == NULL)
		goto no_props;

	direction = pw_properties_parse_int(str);

	if ((str = pw_properties_get(properties, "audio-dsp.maxbuffer")) == NULL)
		goto no_props;

	maxbuffer = pw_properties_parse_int(str);

	dsp = pw_audio_dsp_new(pw_module_get_core(d->module),
			properties,
			direction,
			maxbuffer,
			sizeof(struct node_data));

	if (dsp == NULL)
		goto no_mem;

	nd = pw_audio_dsp_get_user_data(dsp);
	nd->data = d;
	nd->dsp = dsp;
	spa_list_append(&d->node_list, &nd->link);

	pw_node_register(dsp, client, pw_module_get_global(d->module), NULL);
	pw_node_add_listener(dsp, &nd->dsp_listener, &node_events, nd);

	res = pw_global_bind(pw_node_get_global(dsp), client,
			PW_PERM_RWX, PW_VERSION_NODE_PROXY, new_id);
	if (res < 0)
		goto no_bind;

	if ((bound_resource = pw_client_find_resource(client, new_id)) == NULL)
		goto no_bind;

	pw_resource_add_listener(bound_resource, &nd->resource_listener, &resource_events, nd);

	pw_node_set_active(dsp, true);

	if (properties)
		pw_properties_free(properties);

	return dsp;

      no_resource:
	pw_log_error("audio-dsp needs a resource");
	pw_resource_error(resource, -EINVAL, "no resource");
	goto done;
      no_props:
	pw_log_error("audio-dsp needs a property");
	pw_resource_error(resource, -EINVAL, "no property");
	goto done;
      no_mem:
	pw_log_error("can't create node");
	pw_resource_error(resource, -ENOMEM, "no memory");
	goto done;
      no_bind:
	pw_resource_error(resource, res, "can't bind dsp node");
	goto done;
      done:
	if (properties)
		pw_properties_free(properties);
	return NULL;
}

static const struct pw_factory_implementation impl_factory = {
	PW_VERSION_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void module_destroy(void *data)
{
	struct factory_data *d = data;
	struct node_data *nd, *t;

	spa_hook_remove(&d->module_listener);

	spa_list_for_each_safe(nd, t, &d->node_list, link)
		pw_node_destroy(nd->dsp);

	if (d->properties)
		pw_properties_free(d->properties);

	pw_factory_destroy(d->this);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct pw_factory *factory;
	struct factory_data *data;

	factory = pw_factory_new(core,
				 "audio-dsp",
				 PW_TYPE_INTERFACE_Node,
				 PW_VERSION_NODE_PROXY,
				 NULL,
				 sizeof(*data));
	if (factory == NULL)
		return -ENOMEM;

	data = pw_factory_get_user_data(factory);
	data->this = factory;
	data->module = module;
	data->properties = properties;
	spa_list_init(&data->node_list);

	pw_log_debug("module %p: new", module);

	pw_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	pw_factory_register(factory, NULL, pw_module_get_global(module), NULL);

	pw_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}

SPA_EXPORT
int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
