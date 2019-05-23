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
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/eventfd.h>

#include <spa/pod/parser.h>
#include <spa/node/utils.h>
#include <spa/debug/types.h>

#include "pipewire/interfaces.h"
#include "pipewire/private.h"

#include "pipewire/node.h"
#include "pipewire/data-loop.h"
#include "pipewire/main-loop.h"
#include "pipewire/type.h"
#include "pipewire/work-queue.h"

/** \cond */
struct impl {
	struct pw_node this;

	struct pw_work_queue *work;

	int last_error;

	unsigned int pause_on_idle:1;
};

#define pw_node_resource(r,m,v,...)	pw_resource_notify(r,struct pw_node_proxy_events,m,v,__VA_ARGS__)
#define pw_node_resource_info(r,...)	pw_node_resource(r,info,0,__VA_ARGS__)
#define pw_node_resource_param(r,...)	pw_node_resource(r,param,0,__VA_ARGS__)

struct resource_data {
	struct spa_hook resource_listener;
	struct pw_node *node;
	struct pw_resource *resource;
	uint32_t subscribe_ids[MAX_PARAMS];
	uint32_t n_subscribe_ids;
};

/** \endcond */

static void node_deactivate(struct pw_node *this)
{
	struct pw_port *port;
	struct pw_link *link;

	pw_log_debug("node %p: deactivate", this);
	spa_list_for_each(port, &this->input_ports, link) {
		spa_list_for_each(link, &port->links, input_link)
			pw_link_deactivate(link);
	}
	spa_list_for_each(port, &this->output_ports, link) {
		spa_list_for_each(link, &port->links, output_link)
			pw_link_deactivate(link);
	}
}

static void add_node(struct pw_node *this, struct pw_node *driver)
{
	pw_log_trace("node %p: add to driver %p", this, driver);
	/* signal the driver */
	this->rt.driver_target.activation = driver->rt.activation;
	this->rt.driver_target.node = driver;
	this->rt.driver_target.data = driver;
	spa_list_append(&this->rt.target_list, &this->rt.driver_target.link);
	this->rt.driver_target.activation->state[0].required++;

	spa_list_append(&driver->rt.target_list, &this->rt.target.link);
	this->rt.activation->state[0].required++;
}

static void remove_node(struct pw_node *this)
{
	pw_log_trace("node %p: remove from driver %p", this,
				this->rt.driver_target.data);
	spa_list_remove(&this->rt.driver_target.link);
	this->rt.driver_target.activation->state[0].required--;

	spa_list_remove(&this->rt.target.link);
	this->rt.activation->state[0].required--;
}

static int
do_node_remove(struct spa_loop *loop,
	       bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_node *this = user_data;
	if (this->source.loop != NULL) {
		spa_loop_remove_source(loop, &this->source);
		remove_node(this);
	}
	return 0;
}

static int pause_node(struct pw_node *this)
{
	int res = 0;

	if (this->info.state <= PW_NODE_STATE_IDLE)
		return 0;

	pw_log_debug("node %p: pause node", this);
	node_deactivate(this);

	pw_loop_invoke(this->data_loop, do_node_remove, 1, NULL, 0, true, this);

	res = spa_node_send_command(this->node,
				    &SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause));
	if (res < 0)
		pw_log_debug("node %p: pause node error %s", this, spa_strerror(res));

	return res;
}

static int
do_node_add(struct spa_loop *loop,
	    bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_node *this = user_data;
	struct pw_node *driver = this->driver_node;

	if (this->source.loop == NULL) {
		spa_loop_add_source(loop, &this->source);
		add_node(this, driver);
	}
	return 0;
}

static int start_node(struct pw_node *this)
{
	int res = 0;

	if (this->info.state >= PW_NODE_STATE_RUNNING)
		return 0;

	pw_log_debug("node %p: start node %d %d %d %d", this, this->n_ready_output_links,
			this->n_used_output_links, this->n_ready_input_links,
			this->n_used_input_links);

	if (this->n_ready_output_links != this->n_used_output_links ||
	    this->n_ready_input_links != this->n_used_input_links)
		return 0;

	res = spa_node_send_command(this->node,
				    &SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start));

	if (res < 0)
		pw_log_debug("node %p: start node error %s", this, spa_strerror(res));

	return res;
}

static void emit_info_changed(struct pw_node *node)
{
	struct pw_resource *resource;

	if (node->info.change_mask == 0)
		return;

	pw_node_emit_info_changed(node, &node->info);

	if (node->global)
		spa_list_for_each(resource, &node->global->resource_list, link)
			pw_node_resource_info(resource, &node->info);

	node->info.change_mask = 0;
}

static int resource_is_subscribed(struct pw_resource *resource, uint32_t id)
{
	struct resource_data *data = pw_resource_get_user_data(resource);
	uint32_t i;

	for (i = 0; i < data->n_subscribe_ids; i++) {
		if (data->subscribe_ids[i] == id)
			return 1;
	}
	return 0;
}

static int notify_param(void *data, int seq, uint32_t id,
		uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct pw_node *node = data;
	struct pw_resource *resource;

	spa_list_for_each(resource, &node->global->resource_list, link) {
		if (!resource_is_subscribed(resource, id))
			continue;

		pw_log_debug("resource %p: notify param %d", resource, id);
		pw_node_resource_param(resource, seq, id, index, next, param);
	}
	return 0;
}

static void emit_params(struct pw_node *node, uint32_t *changed_ids, uint32_t n_changed_ids)
{
	uint32_t i;
	int res;

	if (node->global == NULL)
		return;

	pw_log_debug("node %p: emit %d params", node, n_changed_ids);

	for (i = 0; i < n_changed_ids; i++) {
		struct pw_resource *resource;
		int subscribed = 0;

		/* first check if anyone is subscribed */
		spa_list_for_each(resource, &node->global->resource_list, link) {
			if ((subscribed = resource_is_subscribed(resource, changed_ids[i])))
				break;
		}
		if (!subscribed)
			continue;

		if ((res = pw_node_for_each_param(node, 1, changed_ids[i], 0, UINT32_MAX,
					NULL, notify_param, node)) < 0) {
			pw_log_error("node %p: error %d (%s)", node, res, spa_strerror(res));
		}
	}
}

static void node_update_state(struct pw_node *node, enum pw_node_state state, char *error)
{
	enum pw_node_state old;

	old = node->info.state;
	if (old == state)
		return;

	if (state == PW_NODE_STATE_ERROR) {
		pw_log_error("node %p: update state from %s -> error (%s)", node,
		     pw_node_state_as_string(old), error);
	} else {
		pw_log_debug("node %p: update state from %s -> %s", node,
		     pw_node_state_as_string(old), pw_node_state_as_string(state));
	}

	free((char*)node->info.error);
	node->info.error = error;
	node->info.state = state;

	switch (state) {
	case PW_NODE_STATE_RUNNING:
		pw_loop_invoke(node->data_loop, do_node_add, 1, NULL, 0, true, node);
		break;
	default:
		break;
	}

	pw_node_emit_state_changed(node, old, state, error);

	node->info.change_mask |= PW_NODE_CHANGE_MASK_STATE;
	emit_info_changed(node);
}

static int suspend_node(struct pw_node *this)
{
	int res = 0;
	struct pw_port *p;

	pw_log_debug("node %p: suspend node", this);

	spa_list_for_each(p, &this->input_ports, link) {
		if ((res = pw_port_set_param(p, SPA_PARAM_Format, 0, NULL)) < 0)
			pw_log_warn("error unset format input: %s", spa_strerror(res));
		/* force CONFIGURE in case of async */
		p->state = PW_PORT_STATE_CONFIGURE;
	}

	spa_list_for_each(p, &this->output_ports, link) {
		if ((res = pw_port_set_param(p, SPA_PARAM_Format, 0, NULL)) < 0)
			pw_log_warn("error unset format output: %s", spa_strerror(res));
		/* force CONFIGURE in case of async */
		p->state = PW_PORT_STATE_CONFIGURE;
	}
	node_update_state(this, PW_NODE_STATE_SUSPENDED, NULL);
	return res;
}
static void node_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static void
clear_info(struct pw_node *this)
{
	free((char*)this->info.name);
	free((char*)this->info.error);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = node_unbind_func,
};

static int reply_param(void *data, int seq, uint32_t id,
		uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct resource_data *d = data;
	pw_log_debug("resource %p: reply param %d", d->resource, seq);
	pw_node_resource_param(d->resource, seq, id, index, next, param);
	return 0;
}

static int node_enum_params(void *object, int seq, uint32_t id,
		uint32_t index, uint32_t num, const struct spa_pod *filter)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_node *node = data->node;
	struct pw_client *client = resource->client;
	int res;

	pw_log_debug("resource %p: enum params %d %s %u %u", resource, seq,
			spa_debug_type_find_name(spa_type_param, id), index, num);

	if ((res = pw_node_for_each_param(node, seq, id, index, num,
				filter, reply_param, data)) < 0) {
		pw_log_error("resource %p: %d error %d (%s)", resource,
				resource->id, res, spa_strerror(res));
		pw_core_resource_errorf(client->core_resource,
				resource->id, seq, res,
				"enum params %s failed",
				spa_debug_type_find_name(spa_type_param, id));
	}
	return 0;
}

static int node_subscribe_params(void *object, uint32_t *ids, uint32_t n_ids)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	uint32_t i;

	n_ids = SPA_MIN(n_ids, SPA_N_ELEMENTS(data->subscribe_ids));
	data->n_subscribe_ids = n_ids;

	for (i = 0; i < n_ids; i++) {
		data->subscribe_ids[i] = ids[i];
		pw_log_debug("resource %p: subscribe param %s", resource,
			spa_debug_type_find_name(spa_type_param, ids[i]));
		node_enum_params(resource, 1, ids[i], 0, UINT32_MAX, NULL);
	}
	return 0;
}

static int node_set_param(void *object, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_node *node = data->node;
	int res;

	pw_log_debug("resource %p: set param %s %08x", resource,
			spa_debug_type_find_name(spa_type_param, id), flags);

	if ((res = spa_node_set_param(node->node, id, flags, param)) < 0) {
		pw_log_error("resource %p: %d error %d (%s)", resource,
				resource->id, res, spa_strerror(res));
		pw_resource_error(resource, res, spa_strerror(res));
	}
	return 0;
}

static int node_send_command(void *object, const struct spa_command *command)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_node *node = data->node;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Suspend:
		suspend_node(node);
		break;
	default:
		spa_node_send_command(node->node, command);
		break;
	}
	return 0;
}

static const struct pw_node_proxy_methods node_methods = {
	PW_VERSION_NODE_PROXY_METHODS,
	.subscribe_params = node_subscribe_params,
	.enum_params = node_enum_params,
	.set_param = node_set_param,
	.send_command = node_send_command
};

static int
global_bind(void *_data, struct pw_client *client, uint32_t permissions,
	    uint32_t version, uint32_t id)
{
	struct pw_node *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	data->node = this;
	data->resource = resource;
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_resource_set_implementation(resource, &node_methods, resource);

	pw_log_debug("node %p: bound to %d", this, resource->id);

	spa_list_append(&global->resource_list, &resource->link);

	this->info.change_mask = PW_NODE_CHANGE_MASK_ALL;
	pw_node_resource_info(resource, &this->info);
	this->info.change_mask = 0;
	return 0;

      no_mem:
	pw_log_error("can't create node resource");
	return -ENOMEM;
}

static void global_destroy(void *data)
{
	struct pw_node *this = data;
	spa_hook_remove(&this->global_listener);
	this->global = NULL;
	pw_node_destroy(this);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

SPA_EXPORT
int pw_node_register(struct pw_node *this,
		     struct pw_client *owner,
		     struct pw_global *parent,
		     struct pw_properties *properties)
{
	struct pw_core *core = this->core;
	struct pw_port *port;
	const char *str;

	pw_log_debug("node %p: register", this);

	if (this->registered)
		return -EEXIST;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return -ENOMEM;

	if ((str = pw_properties_get(this->properties, "media.class")) != NULL)
		pw_properties_set(properties, "media.class", str);
	if ((str = pw_properties_get(this->properties, "media.role")) != NULL)
		pw_properties_set(properties, "media.role", str);
	pw_properties_set(properties, "node.name", this->info.name);
	if ((str = pw_properties_get(this->properties, "node.session")) != NULL)
		pw_properties_set(properties, "node.session", str);

	spa_list_append(&core->node_list, &this->link);
	this->registered = true;

	this->global = pw_global_new(core,
				     PW_TYPE_INTERFACE_Node,
				     PW_VERSION_NODE_PROXY,
				     properties,
				     global_bind,
				     this);
	if (this->global == NULL)
		return -ENOMEM;

	this->info.id = this->global->id;
	this->rt.activation->position.clock.id = this->info.id;
	pw_properties_setf(this->properties, "node.id", "%d", this->info.id);

	pw_node_initialized(this);

	pw_global_add_listener(this->global, &this->global_listener, &global_events, this);
	pw_global_register(this->global, owner, parent);

	spa_list_for_each(port, &this->input_ports, link)
		pw_port_register(port, this->global->owner, this->global,
				 pw_properties_copy(port->properties));
	spa_list_for_each(port, &this->output_ports, link)
		pw_port_register(port, this->global->owner, this->global,
				 pw_properties_copy(port->properties));

	return 0;
}

SPA_EXPORT
int pw_node_initialized(struct pw_node *this)
{
	pw_log_debug("node %p initialized", this);
	pw_node_emit_initialized(this);
	node_update_state(this, PW_NODE_STATE_SUSPENDED, NULL);
	return 0;
}

static int
do_move_nodes(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *src = user_data;
	struct pw_node *driver = *(struct pw_node **)data;
	struct pw_node *this = &src->this;

	pw_log_trace("node %p: driver:%p->%p", this, this->driver_node, driver);

	if (this->source.loop != NULL) {
		remove_node(this);
		add_node(this, driver);
	}
	return 0;
}

SPA_EXPORT
int pw_node_set_driver(struct pw_node *node, struct pw_node *driver)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct pw_node *old = node->driver_node;
	int res;

	if (driver == NULL)
		driver = node;

	if (old == driver)
		return 0;

	node->master = node->driver && driver == node;

	node->driver_node = driver;
	pw_node_emit_driver_changed(node, old, driver);

	if ((res = spa_node_set_io(node->node,
		    SPA_IO_Position,
		    &driver->rt.activation->position,
		    sizeof(struct spa_io_position))) < 0) {
		pw_log_warn("node %p: set position %s", node, spa_strerror(res));
	} else {
		pw_log_trace("node %p: set position %p", node, &driver->rt.activation->position);
		node->rt.position = &driver->rt.activation->position;
	}

	pw_loop_invoke(node->data_loop,
		       do_move_nodes, SPA_ID_INVALID, &driver, sizeof(struct pw_node *),
		       true, impl);
	return 0;
}

static uint32_t flp2(uint32_t x)
{
	x = x | (x >> 1);
	x = x | (x >> 2);
	x = x | (x >> 4);
	x = x | (x >> 8);
	x = x | (x >> 16);
	return x - (x >> 1);
}

static void check_properties(struct pw_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	const char *str;
	bool driver;

	if ((str = pw_properties_get(node->properties, "node.pause-on-idle")))
		impl->pause_on_idle = pw_properties_parse_bool(str);
	else
		impl->pause_on_idle = true;

	if ((str = pw_properties_get(node->properties, "node.driver")))
		driver = pw_properties_parse_bool(str);
	else
		driver = false;

	if (node->driver != driver) {
		pw_log_info("node %p: driver %d -> %d", node, node->driver, driver);
		node->driver = driver;
		if (driver)
			spa_list_append(&node->core->driver_list, &node->driver_link);
		else
			spa_list_remove(&node->driver_link);
	}

	if ((str = pw_properties_get(node->properties, "node.latency"))) {
		uint32_t num, denom;
		pw_log_info("node %p: latency '%s'", node, str);
                if (sscanf(str, "%u/%u", &num, &denom) == 2 && denom != 0) {
			node->quantum_size = flp2((num * 48000 / denom));
			pw_log_info("node %p: quantum %d", node, node->quantum_size);
		}
	} else
		node->quantum_size = DEFAULT_QUANTUM;

	pw_log_debug("node %p: driver:%d", node, node->driver);

}

static void dump_states(struct pw_node *driver)
{
	struct pw_node_target *t;

	spa_list_for_each(t, &driver->rt.target_list, link) {
		struct pw_node_activation *a = t->activation;
		pw_log_warn("node %p (%s): required:%d s:%"PRIu64" a:%"PRIu64" f:%"PRIu64
				" waiting:%"PRIu64" process:%"PRIu64" status:%d",
				t->node, t->node ? t->node->info.name : "",
				a->state[0].required,
				a->signal_time,
				a->awake_time,
				a->finish_time,
				a->awake_time - a->signal_time,
				a->finish_time - a->awake_time,
				t->activation->status);
	}
}

static inline int resume_node(struct pw_node *this, int status)
{
        struct pw_port *p;
	struct pw_node_target *t;
	struct timespec ts;
	struct pw_node_activation *activation = this->rt.activation;
	uint64_t nsec;

	if (status & SPA_STATUS_HAVE_BUFFER) {
		spa_list_for_each(p, &this->rt.output_mix, rt.node_link)
			spa_node_process(p->mix);
	}

	clock_gettime(CLOCK_MONOTONIC, &ts);
	nsec = SPA_TIMESPEC_TO_NSEC(&ts);
	activation->status = FINISHED;
	activation->finish_time = nsec;

        pw_log_trace_fp("node %p: trigger peers", this);

	spa_list_for_each(t, &this->rt.target_list, link) {
		struct pw_node_activation_state *state;

		state = &t->activation->state[0];

		pw_log_trace_fp("node %p: state %p pending %d/%d", t->node, state,
                                state->pending, state->required);

		if (pw_node_activation_state_dec(state, 1)) {
			t->activation->status = TRIGGERED;
			t->activation->signal_time = nsec;
			t->signal(t->data);
		}
	}
	return 0;
}

static inline int process_node(void *data)
{
	struct pw_node *this = data;
	struct timespec ts;
        struct pw_port *p;
	struct pw_node_activation *a = this->rt.activation;
	int status;

        pw_log_trace_fp("node %p: process", this);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	a->status = AWAKE;
	a->awake_time = SPA_TIMESPEC_TO_NSEC(&ts);

	spa_list_for_each(p, &this->rt.input_mix, rt.node_link)
		spa_node_process(p->mix);

	status = spa_node_process(this->node);
	a->state[0].status = status;

	if (this == this->driver_node && !this->exported) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		a->status = FINISHED;
		a->signal_time = a->finish_time;
		a->finish_time = SPA_TIMESPEC_TO_NSEC(&ts);
		a->running = false;
		pw_log_trace_fp("node %p: graph completed wait:%"PRIu64" run:%"PRIu64, this,
				a->awake_time - a->signal_time,
				a->finish_time - a->awake_time);
	} else if (status == SPA_STATUS_OK) {
		pw_log_trace_fp("node %p: async continue", this);
	} else {
		resume_node(this, status);
	}
	return 0;
}

static void node_on_fd_events(struct spa_source *source)
{
	struct pw_node *this = source->data;

	if (source->rmask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("node %p: got socket error %08x", this, source->rmask);
		return;
	}

	if (source->rmask & SPA_IO_IN) {
		uint64_t cmd;

		if (read(this->source.fd, &cmd, sizeof(cmd)) != sizeof(cmd) || cmd != 1)
			pw_log_warn("node %p: read %"PRIu64" failed %m", this, cmd);

		pw_log_trace_fp("node %p: got process", this);
		this->rt.target.signal(this->rt.target.data);
	}
}

SPA_EXPORT
struct pw_node *pw_node_new(struct pw_core *core,
			    const char *name,
			    struct pw_properties *properties,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_node *this;
	size_t size;
	char *n;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		goto error;

	if (name == NULL)
		asprintf(&n, "node");
	else
		n = strdup(name);

	this = &impl->this;
	this->core = core;
	pw_log_debug("node %p: new \"%s\"", this, n);

	if (user_data_size > 0)
                this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto clean_impl;

	this->properties = properties;

	size = sizeof(struct pw_node_activation);

	this->source.fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (this->source.fd == -1)
		goto clean_impl;

	this->source.func = node_on_fd_events;
	this->source.data = this;
	this->source.mask = SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP;
	this->source.rmask = 0;

	if (pw_memblock_alloc(PW_MEMBLOCK_FLAG_WITH_FD |
			      PW_MEMBLOCK_FLAG_MAP_READWRITE |
			      PW_MEMBLOCK_FLAG_SEAL,
			      size,
			      &this->activation) < 0)
                goto clean_impl;

	impl->work = pw_work_queue_new(this->core->main_loop);
	if (impl->work == NULL)
		goto clean_impl;

	this->info.name = n;

	this->data_loop = core->data_loop;

	spa_list_init(&this->slave_list);

	spa_hook_list_init(&this->listener_list);

	this->info.state = PW_NODE_STATE_CREATING;
	this->info.props = &this->properties->dict;
	this->info.params = this->params;

	spa_list_init(&this->input_ports);
	pw_map_init(&this->input_port_map, 64, 64);
	spa_list_init(&this->output_ports);
	pw_map_init(&this->output_port_map, 64, 64);

	spa_list_init(&this->rt.input_mix);
	spa_list_init(&this->rt.output_mix);
	spa_list_init(&this->rt.target_list);

	this->rt.activation = this->activation->ptr;
	this->rt.target.activation = this->rt.activation;
	this->rt.target.node = this;
	this->rt.target.signal = process_node;
	this->rt.target.data = this;
	this->rt.driver_target.signal = process_node;

	this->rt.activation->position.clock.rate = SPA_FRACTION(1, 48000);
	this->rt.activation->position.size = DEFAULT_QUANTUM;

	check_properties(this);

	this->driver_node = this;
	spa_list_append(&this->slave_list, &this->slave_link);
	this->master = true;

	return this;

    clean_impl:
	if (this->source.func != NULL)
		close(this->source.fd);
	if (properties)
		pw_properties_free(properties);
	free(impl);
    error:
	return NULL;
}

SPA_EXPORT
const struct pw_node_info *pw_node_get_info(struct pw_node *node)
{
	return &node->info;
}

SPA_EXPORT
void * pw_node_get_user_data(struct pw_node *node)
{
	return node->user_data;
}

SPA_EXPORT
struct pw_core * pw_node_get_core(struct pw_node *node)
{
	return node->core;
}

SPA_EXPORT
struct pw_global *pw_node_get_global(struct pw_node *node)
{
	return node->global;
}

SPA_EXPORT
const struct pw_properties *pw_node_get_properties(struct pw_node *node)
{
	return node->properties;
}

static int update_properties(struct pw_node *node, const struct spa_dict *dict)
{
	int changed;

	changed = pw_properties_update(node->properties, dict);

	pw_log_debug("node %p: updated %d properties", node, changed);

	if (changed) {
		check_properties(node);
		node->info.props = &node->properties->dict;
		node->info.change_mask |= PW_NODE_CHANGE_MASK_PROPS;
	}
	return changed;
}

SPA_EXPORT
int pw_node_update_properties(struct pw_node *node, const struct spa_dict *dict)
{
	int changed = update_properties(node, dict);
	emit_info_changed(node);
	return changed;
}

static void node_info(void *data, const struct spa_node_info *info)
{
	struct pw_node *node = data;
	uint32_t changed_ids[MAX_PARAMS], n_changed_ids = 0;

	node->info.max_input_ports = info->max_input_ports;
	node->info.max_output_ports = info->max_output_ports;

	pw_log_debug("node %p: change_mask %08lx max_in:%u max_out:%u",
			node, info->change_mask, info->max_input_ports,
			info->max_output_ports);

	if (info->change_mask & SPA_NODE_CHANGE_MASK_PROPS) {
		update_properties(node, info->props);
	}
	if (info->change_mask & SPA_NODE_CHANGE_MASK_PARAMS) {
		uint32_t i;

		node->info.change_mask |= PW_NODE_CHANGE_MASK_PARAMS;
		node->info.n_params = SPA_MIN(info->n_params, SPA_N_ELEMENTS(node->params));

		for (i = 0; i < node->info.n_params; i++) {
			if (node->info.params[i].flags == info->params[i].flags)
				continue;

			if (info->params[i].flags & SPA_PARAM_INFO_READ)
				changed_ids[n_changed_ids++] = info->params[i].id;

			node->info.params[i] = info->params[i];
		}
	}
	emit_info_changed(node);

	if (info->change_mask & SPA_NODE_CHANGE_MASK_PARAMS)
		emit_params(node, changed_ids, n_changed_ids);
}

static void node_port_info(void *data, enum spa_direction direction, uint32_t port_id,
		const struct spa_port_info *info)
{
	struct pw_node *node = data;
	struct pw_port *port = pw_node_find_port(node, direction, port_id);

	if (info == NULL) {
		if (port) {
			pw_log_debug("node %p: %s port %d removed", node,
					pw_direction_as_string(direction), port_id);
			pw_port_destroy(port);
		} else {
			pw_log_warn("node %p: %s port %d unknown", node,
					pw_direction_as_string(direction), port_id);
		}
	} else if (port) {
		pw_log_debug("node %p: %s port %d changed", node,
				pw_direction_as_string(direction), port_id);
		pw_port_update_info(port, info);
	} else {
		int res;

		pw_log_debug("node %p: %s port %d added", node,
				pw_direction_as_string(direction), port_id);

		if ((port = pw_port_new(direction, port_id, info,
					node->port_user_data_size))) {
			if ((res = pw_port_add(port, node)) < 0) {
				pw_log_error("node %p: can't add port %p: %d, %s",
						node, port, res, spa_strerror(res));
				pw_port_destroy(port);
			}
		}
	}
}

static void node_result(void *data, int seq, int res, const void *result)
{
	struct pw_node *node = data;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);

	pw_log_trace("node %p: result seq:%d res:%d", node, seq, res);
	impl->last_error = res;

	if (SPA_RESULT_IS_ASYNC(seq))
	        pw_work_queue_complete(impl->work, &impl->this, SPA_RESULT_ASYNC_SEQ(seq), res);

	pw_node_emit_result(node, seq, res, result);
}

static void node_event(void *data, struct spa_event *event)
{
	struct pw_node *node = data;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);

	pw_log_trace("node %p: event %d", node, SPA_EVENT_TYPE(event));

	switch (SPA_NODE_EVENT_ID(event)) {
	case SPA_NODE_EVENT_Error:
		impl->last_error = -EFAULT;
		node_update_state(node, PW_NODE_STATE_ERROR, strdup("error"));
		break;
	default:
		break;
	}
	pw_node_emit_event(node, event);
}

static const struct spa_node_events node_events = {
	SPA_VERSION_NODE_EVENTS,
	.info = node_info,
	.port_info = node_port_info,
	.result = node_result,
	.event = node_event,
};

static int node_ready(void *data, int status)
{
	struct pw_node *node = data;
	struct pw_node *driver = node->driver_node;
	struct pw_node_target *t;

	pw_log_trace_fp("node %p: ready driver:%d exported:%d %p status:%d", node,
			node->driver, node->exported, driver, status);

	if (node == driver) {
		if (node->rt.activation->running) {
			pw_log_warn("node %p: graph not finished", node);
			dump_states(node);
	                node->rt.target.signal(node->rt.target.data);
		}
		spa_list_for_each(t, &driver->rt.target_list, link) {
			pw_node_activation_state_reset(&t->activation->state[0]);
			t->activation->status = NOT_TRIGGERED;
		}
		node->rt.activation->running = true;
	}
	if (node->driver && !node->master)
		return 0;

	return resume_node(node, status);
}

static int node_reuse_buffer(void *data, uint32_t port_id, uint32_t buffer_id)
{
	struct pw_node *node = data;
	struct pw_port *p;

	spa_list_for_each(p, &node->rt.input_mix, rt.node_link) {
		if (p->port_id != port_id)
			continue;

		spa_node_port_reuse_buffer(p->mix, p->port_id, buffer_id);
		break;
	}
	return 0;
}

static const struct spa_node_callbacks node_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.ready = node_ready,
	.reuse_buffer = node_reuse_buffer,
};

SPA_EXPORT
int pw_node_set_implementation(struct pw_node *node,
			struct spa_node *spa_node)
{
	int res;

	pw_log_debug("node %p: implementation %p", node, spa_node);

	if (node->node) {
		pw_log_error("node %p: implementation existed %p", node, node->node);
		return -EEXIST;
	}

	node->node = spa_node;
	spa_node_set_callbacks(node->node, &node_callbacks, node);
	res = spa_node_add_listener(node->node, &node->listener, &node_events, node);

	if (spa_node_set_io(node->node,
			    SPA_IO_Position,
			    &node->rt.activation->position,
			    sizeof(struct spa_io_position)) >= 0) {
		pw_log_debug("node %p: set position %p", node, &node->rt.activation->position);
		node->rt.position = &node->rt.activation->position;
	}
	if (spa_node_set_io(node->node,
			    SPA_IO_Clock,
			    &node->rt.activation->position.clock,
			    sizeof(struct spa_io_clock)) >= 0) {
		pw_log_debug("node %p: set clock %p", node, &node->rt.activation->position.clock);
		node->rt.clock = &node->rt.activation->position.clock;
	}
	return res;
}

SPA_EXPORT
struct spa_node *pw_node_get_implementation(struct pw_node *node)
{
	return node->node;
}

SPA_EXPORT
void pw_node_add_listener(struct pw_node *node,
			   struct spa_hook *listener,
			   const struct pw_node_events *events,
			   void *data)
{
	spa_hook_list_append(&node->listener_list, listener, events, data);
}

/** Destroy a node
 * \param node a node to destroy
 *
 * Remove \a node. This will stop the transfer on the node and
 * free the resources allocated by \a node.
 *
 * \memberof pw_node
 */
SPA_EXPORT
void pw_node_destroy(struct pw_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct pw_port *port;

	pw_log_debug("node %p: destroy", impl);
	pw_node_emit_destroy(node);

	pause_node(node);
	suspend_node(node);

	pw_log_debug("node %p: driver node %p", impl, node->driver_node);

	if (node->driver)
		spa_list_remove(&node->driver_link);

	/* remove ourself as a slave from the driver node */
	spa_list_remove(&node->slave_link);

	if (node->registered)
		spa_list_remove(&node->link);

	if (node->node) {
		spa_hook_remove(&node->listener);
		spa_node_set_callbacks(node->node, NULL, NULL);
	}

	pw_log_debug("node %p: unlink ports", node);
	spa_list_for_each(port, &node->input_ports, link)
		pw_port_unlink(port);
	spa_list_for_each(port, &node->output_ports, link)
		pw_port_unlink(port);

	pw_log_debug("node %p: destroy ports", node);
	spa_list_consume(port, &node->input_ports, link)
		pw_port_destroy(port);
	spa_list_consume(port, &node->output_ports, link)
		pw_port_destroy(port);

	if (node->global) {
		spa_hook_remove(&node->global_listener);
		pw_global_destroy(node->global);
	}

	pw_core_recalc_graph(node->core);

	pw_log_debug("node %p: free", node);
	pw_node_emit_free(node);

	pw_memblock_free(node->activation);

	pw_work_queue_destroy(impl->work);

	pw_map_clear(&node->input_port_map);
	pw_map_clear(&node->output_port_map);

	pw_properties_free(node->properties);

	clear_info(node);

	close(node->source.fd);
	free(impl);
}

SPA_EXPORT
int pw_node_for_each_port(struct pw_node *node,
			  enum pw_direction direction,
			  int (*callback) (void *data, struct pw_port *port),
			  void *data)
{
	struct spa_list *ports;
	struct pw_port *p, *t;
	int res;

	if (direction == PW_DIRECTION_INPUT)
		ports = &node->input_ports;
	else
		ports = &node->output_ports;

	spa_list_for_each_safe(p, t, ports, link)
		if ((res = callback(data, p)) != 0)
			return res;
	return 0;
}

struct result_node_params_data {
	void *data;
	int (*callback) (void *data, int seq,
			uint32_t id, uint32_t index, uint32_t next,
			struct spa_pod *param);
	int seq;
};

static void result_node_params(void *data, int seq, int res, const void *result)
{
	struct result_node_params_data *d = data;
	const struct spa_result_node_params *r = result;
	if (d->seq == seq)
		d->callback(d->data, seq, r->id, r->index, r->next, r->param);
}

SPA_EXPORT
int pw_node_for_each_param(struct pw_node *node,
			   int seq, uint32_t param_id,
			   uint32_t index, uint32_t max,
			   const struct spa_pod *filter,
			   int (*callback) (void *data, int seq,
					    uint32_t id, uint32_t index, uint32_t next,
					    struct spa_pod *param),
			   void *data)
{
	int res;
	struct result_node_params_data user_data = { data, callback, seq };
	struct spa_hook listener;
	static const struct spa_node_events node_events = {
		SPA_VERSION_NODE_EVENTS,
		.result = result_node_params,
	};

	if (max == 0)
		max = UINT32_MAX;

	pw_log_debug("node %p: params %s %u %u", node,
			spa_debug_type_find_name(spa_type_param, param_id),
			index, max);

	spa_zero(listener);
	spa_node_add_listener(node->node, &listener, &node_events, &user_data);
	res = spa_node_enum_params(node->node, seq,
					param_id, index, max,
					filter);
	spa_hook_remove(&listener);

	return res;
}

SPA_EXPORT
struct pw_port *
pw_node_find_port(struct pw_node *node, enum pw_direction direction, uint32_t port_id)
{
	struct pw_port *port, *p;
	struct pw_map *portmap;
	struct spa_list *ports;

	if (direction == PW_DIRECTION_INPUT) {
		portmap = &node->input_port_map;
		ports = &node->input_ports;
	} else {
		portmap = &node->output_port_map;
		ports = &node->output_ports;
	}

	if (port_id != SPA_ID_INVALID)
		port = pw_map_lookup(portmap, port_id);
	else {
		port = NULL;
		/* try to find an unlinked port */
		spa_list_for_each(p, ports, link) {
			if (spa_list_is_empty(&p->links)) {
				port = p;
				break;
			}
			/* We can use this port if it can multiplex */
			if (SPA_FLAG_CHECK(p->mix_flags, PW_PORT_MIX_FLAG_MULTI))
				port = p;
		}
	}
	pw_log_debug("node %p: return %s port %d: %p", node,
			pw_direction_as_string(direction), port_id, port);
	return port;
}

SPA_EXPORT
uint32_t pw_node_get_free_port_id(struct pw_node *node, enum pw_direction direction)
{
	uint32_t n_ports, max_ports;
	struct pw_map *portmap;
	uint32_t port_id;

	if (direction == PW_DIRECTION_INPUT) {
		max_ports = node->info.max_input_ports;
		n_ports = node->info.n_input_ports;
		portmap = &node->input_port_map;
	} else {
		max_ports = node->info.max_output_ports;
		n_ports = node->info.n_output_ports;
		portmap = &node->output_port_map;
	}
	pw_log_debug("node %p: direction %s n_ports:%u max_ports:%u",
			node, pw_direction_as_string(direction), n_ports, max_ports);

	if (n_ports >= max_ports)
		goto no_mem;

	port_id = pw_map_insert_new(portmap, NULL);
	if (port_id == SPA_ID_INVALID)
		goto no_mem;

	pw_log_debug("node %p: free port %d", node, port_id);

	return port_id;

      no_mem:
	pw_log_warn("no more port available");
	return SPA_ID_INVALID;
}

static void on_state_complete(void *obj, void *data, int res, uint32_t seq)
{
	struct pw_node *node = obj;
	enum pw_node_state state = SPA_PTR_TO_INT(data);
	char *error = NULL;

	pw_log_debug("node %p: state complete %d", node, res);
	if (SPA_RESULT_IS_ERROR(res)) {
		asprintf(&error, "error changing node state: %d", res);
		state = PW_NODE_STATE_ERROR;
	}
	node_update_state(node, state, error);
}

static void node_activate(struct pw_node *this)
{
	struct pw_port *port;

	pw_log_debug("node %p: activate", this);
	spa_list_for_each(port, &this->input_ports, link) {
		struct pw_link *link;
		spa_list_for_each(link, &port->links, input_link)
			pw_link_activate(link);
	}
	spa_list_for_each(port, &this->output_ports, link) {
		struct pw_link *link;
		spa_list_for_each(link, &port->links, output_link)
			pw_link_activate(link);
	}
}

/** Set th node state
 * \param node a \ref pw_node
 * \param state a \ref pw_node_state
 * \return 0 on success < 0 on error
 *
 * Set the state of \a node to \a state.
 *
 * \memberof pw_node
 */
SPA_EXPORT
int pw_node_set_state(struct pw_node *node, enum pw_node_state state)
{
	int res = 0;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	enum pw_node_state old = node->info.state;

	pw_log_debug("node %p: set state %s -> %s, active %d", node,
			pw_node_state_as_string(old),
			pw_node_state_as_string(state),
			node->active);

	if (old == state)
		return 0;

	pw_node_emit_state_request(node, state);

	switch (state) {
	case PW_NODE_STATE_CREATING:
		return -EIO;

	case PW_NODE_STATE_SUSPENDED:
		res = suspend_node(node);
		break;

	case PW_NODE_STATE_IDLE:
		if (node->active && impl->pause_on_idle)
			res = pause_node(node);
		break;

	case PW_NODE_STATE_RUNNING:
		if (node->active) {
			node_activate(node);
			res = start_node(node);
		}
		break;

	case PW_NODE_STATE_ERROR:
		break;
	}
	if (SPA_RESULT_IS_ERROR(res))
		return res;

	if (SPA_RESULT_IS_ASYNC(res)) {
		res = spa_node_sync(node->node, res);
	}

	pw_work_queue_add(impl->work,
			  node, res, on_state_complete, SPA_INT_TO_PTR(state));

	return res;
}

SPA_EXPORT
int pw_node_set_active(struct pw_node *node, bool active)
{
	bool old = node->active;

	if (old != active) {
		pw_log_debug("node %p: %s", node, active ? "activate" : "deactivate");

		if (!active)
			pw_node_set_state(node, PW_NODE_STATE_IDLE);

		node->active = active;
		pw_node_emit_active_changed(node, active);

		if (active)
			node_activate(node);

		pw_core_recalc_graph(node->core);
	}
	return 0;
}

SPA_EXPORT
bool pw_node_is_active(struct pw_node *node)
{
	return node->active;
}
