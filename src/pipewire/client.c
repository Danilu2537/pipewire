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

#include <errno.h>
#include <string.h>

#include "pipewire/interfaces.h"
#include "pipewire/client.h"
#include "pipewire/private.h"
#include "pipewire/resource.h"
#include "pipewire/type.h"

/** \cond */
struct impl {
	struct pw_client this;
	struct spa_hook core_listener;
	struct pw_array permissions;
};

#define pw_client_resource(r,m,v,...)	pw_resource_notify(r,struct pw_client_proxy_events,m,v,__VA_ARGS__)
#define pw_client_resource_info(r,...)		pw_client_resource(r,info,0,__VA_ARGS__)
#define pw_client_resource_permissions(r,...)	pw_client_resource(r,permissions,0,__VA_ARGS__)

struct resource_data {
	struct spa_hook resource_listener;
	struct pw_client *client;
};

/** find a specific permission for a global or the default when there is none */
static struct pw_permission *
find_permission(struct pw_client *client, uint32_t id)
{
	struct impl *impl = SPA_CONTAINER_OF(client, struct impl, this);
	struct pw_permission *p;
	uint32_t idx = id + 1;

	if (id == SPA_ID_INVALID)
		goto do_default;

	if (!pw_array_check_index(&impl->permissions, idx, struct pw_permission))
		goto do_default;

	p = pw_array_get_unchecked(&impl->permissions, idx, struct pw_permission);
	if (p->permissions == SPA_ID_INVALID)
		goto do_default;

	return p;

      do_default:
	return pw_array_get_unchecked(&impl->permissions, 0, struct pw_permission);
}

static struct pw_permission *ensure_permissions(struct pw_client *client, uint32_t id)
{
	struct impl *impl = SPA_CONTAINER_OF(client, struct impl, this);
	struct pw_permission *p;
	uint32_t idx = id + 1;
	size_t len, i;

	len = pw_array_get_len(&impl->permissions, struct pw_permission);
	if (len <= idx) {
		size_t diff = idx - len + 1;

		p = pw_array_add(&impl->permissions, diff * sizeof(struct pw_permission));
		if (p == NULL)
			return NULL;

		for (i = 0; i < diff; i++) {
			p[i].id = len + i - 1;
			p[i].permissions = SPA_ID_INVALID;
		}
	}
	p = pw_array_get_unchecked(&impl->permissions, idx, struct pw_permission);
	return p;
}

/** \endcond */

static uint32_t
client_permission_func(struct pw_global *global,
		       struct pw_client *client, void *data)
{
	struct pw_permission *p;
	p = find_permission(client, global->id);
	return p->permissions;
}

static int client_error(void *object, uint32_t id, int res, const char *error)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_client *client = data->client;
	struct pw_global *global;
	struct pw_resource *r, *t;

	global = pw_core_find_global(client->core, id);
	if (global == NULL)
		return -ENOENT;

	spa_list_for_each_safe(r, t, &global->resource_list, link) {
		if (t->client != client)
			continue;
		pw_resource_error(r, res, error);
	}
	return 0;
}

static int client_update_properties(void *object, const struct spa_dict *props)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_client *client = data->client;
	return pw_client_update_properties(client, props);
}

static int client_get_permissions(void *object, uint32_t index, uint32_t num)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_client *client = data->client;
	struct impl *impl = SPA_CONTAINER_OF(client, struct impl, this);
	size_t len;

	len = pw_array_get_len(&impl->permissions, struct pw_permission);
	if ((size_t)index >= len)
		num = 0;
	else if ((size_t)index + (size_t)num >= len)
		num = len - index;

	pw_client_resource_permissions(resource, index,
			num, pw_array_get_unchecked(&impl->permissions, index, struct pw_permission));
	return 0;
}

static int client_update_permissions(void *object,
		uint32_t n_permissions, const struct pw_permission *permissions)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_client *client = data->client;
	return pw_client_update_permissions(client, n_permissions, permissions);
}

static const struct pw_client_proxy_methods client_methods = {
	PW_VERSION_CLIENT_PROXY_METHODS,
	.error = client_error,
	.update_properties = client_update_properties,
	.get_permissions = client_get_permissions,
	.update_permissions = client_update_permissions
};

static void client_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	if (resource->id == 1)
		resource->client->client_resource = NULL;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = client_unbind_func,
};

static int
global_bind(void *_data, struct pw_client *client, uint32_t permissions,
		 uint32_t version, uint32_t id)
{
	struct pw_client *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	data->client = this;
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);
	pw_resource_set_implementation(resource, &client_methods, resource);

	pw_log_debug("client %p: bound to %d", this, resource->id);

	spa_list_append(&global->resource_list, &resource->link);

	if (resource->id == 1)
		client->client_resource = resource;

	this->info.change_mask = ~0;
	pw_client_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

      no_mem:
	pw_log_error("can't create client resource");
	return -ENOMEM;
}

static void
core_global_removed(void *data, struct pw_global *global)
{
	struct impl *impl = data;
	struct pw_client *client = &impl->this;
	struct pw_permission *p;

	p = find_permission(client, global->id);
	pw_log_debug("client %p: global %d removed, %p", client, global->id, p);
	if (p->id != SPA_ID_INVALID)
		p->permissions = SPA_ID_INVALID;
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.global_removed = core_global_removed,
};

/** Make a new client object
 *
 * \param core a \ref pw_core object to register the client with
 * \param ucred a ucred structure or NULL when unknown
 * \param properties optional client properties, ownership is taken
 * \return a newly allocated client object
 *
 * \memberof pw_client
 */
SPA_EXPORT
struct pw_client *pw_client_new(struct pw_core *core,
				struct pw_properties *properties,
				size_t user_data_size)
{
	struct pw_client *this;
	struct impl *impl;
	struct pw_permission *p;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("client %p: new", this);

	this->core = core;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return NULL;

	pw_array_init(&impl->permissions, 1024);
	p = pw_array_add(&impl->permissions, sizeof(struct pw_permission));
	p->id = SPA_ID_INVALID;
	p->permissions = 0;

	this->properties = properties;
	this->permission_func = client_permission_func;
	this->permission_data = impl;

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	spa_hook_list_init(&this->listener_list);

	pw_map_init(&this->objects, 0, 32);

	pw_core_add_listener(core, &impl->core_listener, &core_events, impl);

	this->info.props = &this->properties->dict;

	pw_core_emit_check_access(core, this);

	return this;
}

static void global_destroy(void *object)
{
	struct pw_client *client = object;
	spa_hook_remove(&client->global_listener);
	client->global = NULL;
	pw_client_destroy(client);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

SPA_EXPORT
int pw_client_register(struct pw_client *client,
		       struct pw_client *owner,
		       struct pw_global *parent,
		       struct pw_properties *properties)
{
	struct pw_core *core = client->core;

	if (client->registered)
		return -EEXIST;

	pw_log_debug("client %p: register parent %d", client, parent ? parent->id : SPA_ID_INVALID);

	spa_list_append(&core->client_list, &client->link);
	client->registered = true;

	client->global = pw_global_new(core,
				       PW_TYPE_INTERFACE_Client,
				       PW_VERSION_CLIENT_PROXY,
				       properties,
				       global_bind,
				       client);
	if (client->global == NULL)
		return -ENOMEM;

	pw_global_add_listener(client->global, &client->global_listener, &global_events, client);
	pw_global_register(client->global, owner, parent);
	client->info.id = client->global->id;

	return 0;
}

SPA_EXPORT
struct pw_core *pw_client_get_core(struct pw_client *client)
{
	return client->core;
}

SPA_EXPORT
struct pw_resource *pw_client_get_core_resource(struct pw_client *client)
{
	return client->core_resource;
}

SPA_EXPORT
struct pw_resource *pw_client_find_resource(struct pw_client *client, uint32_t id)
{
	return pw_map_lookup(&client->objects, id);
}

SPA_EXPORT
struct pw_global *pw_client_get_global(struct pw_client *client)
{
	return client->global;
}

SPA_EXPORT
const struct pw_properties *pw_client_get_properties(struct pw_client *client)
{
	return client->properties;
}

SPA_EXPORT
void *pw_client_get_user_data(struct pw_client *client)
{
	return client->user_data;
}

static int destroy_resource(void *object, void *data)
{
	if (object)
		pw_resource_destroy(object);
	return 0;
}


/** Destroy a client object
 *
 * \param client the client to destroy
 *
 * \memberof pw_client
 */
SPA_EXPORT
void pw_client_destroy(struct pw_client *client)
{
	struct impl *impl = SPA_CONTAINER_OF(client, struct impl, this);

	pw_log_debug("client %p: destroy", client);
	pw_client_emit_destroy(client);

	spa_hook_remove(&impl->core_listener);

	if (client->registered)
		spa_list_remove(&client->link);

	pw_map_for_each(&client->objects, destroy_resource, client);

	if (client->global) {
		spa_hook_remove(&client->global_listener);
		pw_global_destroy(client->global);
	}

	pw_log_debug("client %p: free", impl);
	pw_client_emit_free(client);

	pw_map_clear(&client->objects);
	pw_array_clear(&impl->permissions);

	pw_properties_free(client->properties);

	free(impl);
}

SPA_EXPORT
void pw_client_add_listener(struct pw_client *client,
			    struct spa_hook *listener,
			    const struct pw_client_events *events,
			    void *data)
{
	spa_hook_list_append(&client->listener_list, listener, events, data);
}

SPA_EXPORT
const struct pw_client_info *pw_client_get_info(struct pw_client *client)
{
	return &client->info;
}

/** Update client properties
 *
 * \param client the client
 * \param dict a \ref spa_dict with properties
 *
 * Add all properties in \a dict to the client properties. Existing
 * properties are overwritten. Items can be removed by setting the value
 * to NULL.
 *
 * \memberof pw_client
 */
SPA_EXPORT
int pw_client_update_properties(struct pw_client *client, const struct spa_dict *dict)
{
	struct pw_resource *resource;
	int changed;

	changed = pw_properties_update(client->properties, dict);

	pw_log_debug("client %p: updated %d properties", client, changed);

	if (!changed)
		return 0;

	client->info.change_mask |= PW_CLIENT_CHANGE_MASK_PROPS;
	client->info.props = &client->properties->dict;

	pw_client_emit_info_changed(client, &client->info);

	if (client->global)
		spa_list_for_each(resource, &client->global->resource_list, link)
			pw_client_resource_info(resource, &client->info);

	client->info.change_mask = 0;

	return changed;
}

SPA_EXPORT
int pw_client_update_permissions(struct pw_client *client,
		uint32_t n_permissions, const struct pw_permission *permissions)
{
	struct pw_core *core = client->core;
	struct pw_permission *def;
	uint32_t i;

	if ((def = find_permission(client, SPA_ID_INVALID)) == NULL)
		return -EIO;

	for (i = 0; i < n_permissions; i++) {
		struct pw_permission *p;
		uint32_t old_perm, new_perm;
		struct pw_global *global;

		if (permissions[i].id == SPA_ID_INVALID) {
			old_perm = def->permissions;
			new_perm = permissions[i].permissions;

			if (core->current_client == client)
				new_perm &= old_perm;

			pw_log_debug("client %p: set default permissions %08x -> %08x",
					client, old_perm, new_perm);

			def->permissions = new_perm;

			spa_list_for_each(global, &core->global_list, link) {
				p = find_permission(client, global->id);
				if (p->id != SPA_ID_INVALID)
					continue;
				pw_global_update_permissions(global, client, old_perm, new_perm);
			}
		}
		else  {
			struct pw_global *global;

			global = pw_core_find_global(client->core, permissions[i].id);
			if (global == NULL || global->id != permissions[i].id) {
				pw_log_warn("client %p: invalid global %d", client, permissions[i].id);
				continue;
			}
			p = ensure_permissions(client, permissions[i].id);
			old_perm = p->permissions == SPA_ID_INVALID ? def->permissions : p->permissions;
			new_perm = permissions[i].permissions;

			if (core->current_client == client)
				new_perm &= old_perm;

			pw_log_debug("client %p: set global %d permissions %08x -> %08x",
					client, global->id, old_perm, new_perm);

			p->permissions = new_perm;
			pw_global_update_permissions(global, client, old_perm, new_perm);
		}
	}
	if (n_permissions > 0)
		pw_client_set_busy(client, false);

	return 0;
}

SPA_EXPORT
void pw_client_set_busy(struct pw_client *client, bool busy)
{
	if (client->busy != busy) {
		pw_log_debug("client %p: busy %d", client, busy);
		client->busy = busy;
		pw_client_emit_busy_changed(client, busy);
	}
}
