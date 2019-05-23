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

#include <stdio.h>
#include <signal.h>

#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>

#include <pipewire/interfaces.h>
#include <pipewire/type.h>
#include <pipewire/remote.h>
#include <pipewire/main-loop.h>
#include <pipewire/pipewire.h>

struct proxy_data;

typedef void (*print_func_t) (struct proxy_data *data);

struct param {
	struct spa_list link;
	uint32_t id;
	int seq;
	struct spa_pod *param;
};

struct data {
	struct pw_main_loop *loop;
	struct pw_core *core;

	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_core_proxy *core_proxy;
	struct spa_hook core_listener;

	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;

	struct spa_list pending_list;
};

struct proxy_data {
	struct data *data;
	bool first;
	struct pw_proxy *proxy;
	uint32_t id;
	uint32_t parent_id;
	uint32_t permissions;
	uint32_t version;
	uint32_t type;
	void *info;
	pw_destroy_t destroy;
	struct spa_hook proxy_listener;
	struct spa_hook proxy_proxy_listener;
	int pending_seq;
	struct spa_list pending_link;
	print_func_t print_func;
	struct spa_list param_list;
};

static void add_pending(struct proxy_data *pd)
{
	struct data *d = pd->data;

	if (pd->pending_seq == 0) {
		spa_list_append(&d->pending_list, &pd->pending_link);
	}
	pd->pending_seq = pw_core_proxy_sync(d->core_proxy, 0, pd->pending_seq);
}

static void remove_pending(struct proxy_data *pd)
{
	if (pd->pending_seq != 0) {
		spa_list_remove(&pd->pending_link);
		pd->pending_seq = 0;
	}
}

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct data *d = data;
	struct proxy_data *pd, *t;

	spa_list_for_each_safe(pd, t, &d->pending_list, pending_link) {
		if (pd->pending_seq == seq) {
			remove_pending(pd);
			pd->print_func(pd);
		}
	}
}

static void clear_params(struct proxy_data *data)
{
	struct param *p;
	spa_list_consume(p, &data->param_list, link) {
		spa_list_remove(&p->link);
		free(p);
	}
}

static void remove_params(struct proxy_data *data, uint32_t id, int seq)
{
	struct param *p, *t;

	spa_list_for_each_safe(p, t, &data->param_list, link) {
		if (p->id == id && seq != p->seq) {
			spa_list_remove(&p->link);
			free(p);
		}
	}
}

static void event_param(void *object, int seq, uint32_t id,
		uint32_t index, uint32_t next, const struct spa_pod *param)
{
        struct proxy_data *data = object;
	struct param *p;

	/* remove all params with the same id and older seq */
	remove_params(data, id, seq);

	/* add new param */
	p = malloc(sizeof(struct param) + SPA_POD_SIZE(param));
	if (p == NULL) {
		pw_log_error("can't add param: %m");
		return;
	}

	p->id = id;
	p->seq = seq;
	p->param = SPA_MEMBER(p, sizeof(struct param), struct spa_pod);
	memcpy(p->param, param, SPA_POD_SIZE(param));
	spa_list_append(&data->param_list, &p->link);
}

static void print_params(struct proxy_data *data, char mark)
{
	struct param *p;

	printf("%c\tparams:\n", mark);
	spa_list_for_each(p, &data->param_list, link) {
		printf("%c\t  id:%u\n", mark, p->id);
		if (spa_pod_is_object_type(p->param, SPA_TYPE_OBJECT_Format))
			spa_debug_format(10, NULL, p->param);
		else
			spa_debug_pod(10, NULL, p->param);
	}
}

static void print_properties(const struct spa_dict *props, char mark)
{
	const struct spa_dict_item *item;

	printf("%c\tproperties:\n", mark);
	if (props == NULL || props->n_items == 0) {
		printf("\t\tnone\n");
		return;
	}

	spa_dict_for_each(item, props) {
		if (item->value)
			printf("%c\t\t%s = \"%s\"\n", mark, item->key, item->value);
		else
			printf("%c\t\t%s = (null)\n", mark, item->key);
	}
}

#define MARK_CHANGE(f) ((print_mark && ((info)->change_mask & (1 << (f)))) ? '*' : ' ')

static void on_core_info(void *data, const struct pw_core_info *info)
{
	bool print_all = true, print_mark = false;

	printf("\ttype: %s\n", spa_debug_type_find_name(pw_type_info(), PW_TYPE_INTERFACE_Core));
	printf("\tcookie: %u\n", info->cookie);
	if (print_all) {
		printf("%c\tuser-name: \"%s\"\n", MARK_CHANGE(0), info->user_name);
		printf("%c\thost-name: \"%s\"\n", MARK_CHANGE(1), info->host_name);
		printf("%c\tversion: \"%s\"\n", MARK_CHANGE(2), info->version);
		printf("%c\tname: \"%s\"\n", MARK_CHANGE(3), info->name);
		print_properties(info->props, MARK_CHANGE(4));
	}
}

static void module_event_info(void *object, const struct pw_module_info *info)
{
        struct proxy_data *data = object;
	bool print_all, print_mark;

	print_all = true;
        if (data->info == NULL) {
		printf("added:\n");
		print_mark = false;
	}
        else {
		printf("changed:\n");
		print_mark = true;
	}

	info = data->info = pw_module_info_update(data->info, info);

	printf("\tid: %d\n", data->id);
	printf("\tparent_id: %d\n", data->parent_id);
	printf("\tpermissions: %c%c%c\n", data->permissions & PW_PERM_R ? 'r' : '-',
					  data->permissions & PW_PERM_W ? 'w' : '-',
					  data->permissions & PW_PERM_X ? 'x' : '-');
	printf("\ttype: %s (version %d)\n",
			spa_debug_type_find_name(pw_type_info(), data->type), data->version);
	if (print_all) {
		printf("%c\tname: \"%s\"\n", MARK_CHANGE(0), info->name);
		printf("%c\tfilename: \"%s\"\n", MARK_CHANGE(1), info->filename);
		printf("%c\targs: \"%s\"\n", MARK_CHANGE(2), info->args);
		print_properties(info->props, MARK_CHANGE(3));
	}
}

static const struct pw_module_proxy_events module_events = {
	PW_VERSION_MODULE_PROXY_EVENTS,
        .info = module_event_info,
};

static void print_node(struct proxy_data *data)
{
	struct pw_node_info *info = data->info;
	bool print_all, print_mark;

	print_all = true;
        if (data->first) {
		printf("added:\n");
		print_mark = false;
		data->first = false;
	}
        else {
		printf("changed:\n");
		print_mark = true;
	}

	printf("\tid: %d\n", data->id);
	printf("\tparent_id: %d\n", data->parent_id);
	printf("\tpermissions: %c%c%c\n", data->permissions & PW_PERM_R ? 'r' : '-',
					  data->permissions & PW_PERM_W ? 'w' : '-',
					  data->permissions & PW_PERM_X ? 'x' : '-');
	printf("\ttype: %s (version %d)\n",
			spa_debug_type_find_name(pw_type_info(), data->type), data->version);
	if (print_all) {
		printf("%c\tname: \"%s\"\n", MARK_CHANGE(0), info->name);
		print_params(data, MARK_CHANGE(5));
		printf("%c\tinput ports: %u/%u\n", MARK_CHANGE(1),
				info->n_input_ports, info->max_input_ports);
		printf("%c\toutput ports: %u/%u\n", MARK_CHANGE(2),
				info->n_output_ports, info->max_output_ports);
		printf("%c\tstate: \"%s\"", MARK_CHANGE(3), pw_node_state_as_string(info->state));
		if (info->state == PW_NODE_STATE_ERROR && info->error)
			printf(" \"%s\"\n", info->error);
		else
			printf("\n");
		print_properties(info->props, MARK_CHANGE(4));


	}
}

static void node_event_info(void *object, const struct pw_node_info *info)
{
        struct proxy_data *data = object;
	struct pw_node_info *old = data->info;
	uint32_t i;

	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if (old != NULL && info->params[i].flags == old->params[i].flags)
				continue;
			remove_params(data, info->params[i].id, 0);
			if (!SPA_FLAG_CHECK(info->params[i].flags, SPA_PARAM_INFO_READ))
				continue;
			pw_node_proxy_enum_params((struct pw_node_proxy*)data->proxy,
					0, info->params[i].id, 0, 0, NULL);
		}
		add_pending(data);
	}
	data->info = pw_node_info_update(data->info, info);

	if (data->pending_seq == 0)
		data->print_func(data);
}

static const struct pw_node_proxy_events node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
        .info = node_event_info,
        .param = event_param
};

static void print_port(struct proxy_data *data)
{
	struct pw_port_info *info = data->info;
	bool print_all, print_mark;

	print_all = true;
        if (data->first) {
		printf("added:\n");
		print_mark = false;
		data->first = false;
	}
        else {
		printf("changed:\n");
		print_mark = true;
	}

	printf("\tid: %d\n", data->id);
	printf("\tparent_id: %d\n", data->parent_id);
	printf("\tpermissions: %c%c%c\n", data->permissions & PW_PERM_R ? 'r' : '-',
					  data->permissions & PW_PERM_W ? 'w' : '-',
					  data->permissions & PW_PERM_X ? 'x' : '-');
	printf("\ttype: %s (version %d)\n",
			spa_debug_type_find_name(pw_type_info(), data->type), data->version);
	if (print_all) {
		printf(" \tdirection: \"%s\"\n", pw_direction_as_string(info->direction));
		print_params(data, MARK_CHANGE(1));
		print_properties(info->props, MARK_CHANGE(0));
	}
}

static void port_event_info(void *object, const struct pw_port_info *info)
{

        struct proxy_data *data = object;
	struct pw_port_info *old = data->info;
	uint32_t i;

	if (info->change_mask & PW_PORT_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if (old != NULL && info->params[i].flags == old->params[i].flags)
				continue;
			remove_params(data, info->params[i].id, 0);
			if (!SPA_FLAG_CHECK(info->params[i].flags, SPA_PARAM_INFO_READ))
				continue;
			pw_port_proxy_enum_params((struct pw_port_proxy*)data->proxy,
					0, info->params[i].id, 0, 0, NULL);
		}
		add_pending(data);
	}
	data->info = pw_port_info_update(data->info, info);

	if (data->pending_seq == 0)
		data->print_func(data);
}

static const struct pw_port_proxy_events port_events = {
	PW_VERSION_PORT_PROXY_EVENTS,
        .info = port_event_info,
        .param = event_param
};

static void factory_event_info(void *object, const struct pw_factory_info *info)
{
        struct proxy_data *data = object;
	bool print_all, print_mark;

	print_all = true;
        if (data->info == NULL) {
		printf("added:\n");
		print_mark = false;
	}
        else {
		printf("changed:\n");
		print_mark = true;
	}

        info = data->info = pw_factory_info_update(data->info, info);

	printf("\tid: %d\n", data->id);
	printf("\tparent_id: %d\n", data->parent_id);
	printf("\tpermissions: %c%c%c\n", data->permissions & PW_PERM_R ? 'r' : '-',
					  data->permissions & PW_PERM_W ? 'w' : '-',
					  data->permissions & PW_PERM_X ? 'x' : '-');
	printf("\ttype: %s (version %d)\n",
			spa_debug_type_find_name(pw_type_info(), data->type), data->version);
	printf("\tname: \"%s\"\n", info->name);
	printf("\tobject-type: %s/%d\n", spa_debug_type_find_name(pw_type_info(), info->type), info->version);
	if (print_all) {
		print_properties(info->props, MARK_CHANGE(0));
	}
}

static const struct pw_factory_proxy_events factory_events = {
	PW_VERSION_FACTORY_PROXY_EVENTS,
        .info = factory_event_info
};

static void client_event_info(void *object, const struct pw_client_info *info)
{
        struct proxy_data *data = object;
	bool print_all, print_mark;

	print_all = true;
        if (data->info == NULL) {
		printf("added:\n");
		print_mark = false;
	}
        else {
		printf("changed:\n");
		print_mark = true;
	}

        info = data->info = pw_client_info_update(data->info, info);

	printf("\tid: %d\n", data->id);
	printf("\tparent_id: %d\n", data->parent_id);
	printf("\tpermissions: %c%c%c\n", data->permissions & PW_PERM_R ? 'r' : '-',
					  data->permissions & PW_PERM_W ? 'w' : '-',
					  data->permissions & PW_PERM_X ? 'x' : '-');
	printf("\ttype: %s (version %d)\n",
			spa_debug_type_find_name(pw_type_info(), data->type), data->version);
	if (print_all) {
		print_properties(info->props, MARK_CHANGE(0));
	}
}

static const struct pw_client_proxy_events client_events = {
	PW_VERSION_CLIENT_PROXY_EVENTS,
        .info = client_event_info
};

static void link_event_info(void *object, const struct pw_link_info *info)
{
        struct proxy_data *data = object;
	bool print_all, print_mark;

	print_all = true;
        if (data->info == NULL) {
		printf("added:\n");
		print_mark = false;
	}
        else {
		printf("changed:\n");
		print_mark = true;
	}

        info = data->info = pw_link_info_update(data->info, info);

	printf("\tid: %d\n", data->id);
	printf("\tparent_id: %d\n", data->parent_id);
	printf("\tpermissions: %c%c%c\n", data->permissions & PW_PERM_R ? 'r' : '-',
					  data->permissions & PW_PERM_W ? 'w' : '-',
					  data->permissions & PW_PERM_X ? 'x' : '-');
	printf("\ttype: %s (version %d)\n",
			spa_debug_type_find_name(pw_type_info(), data->type), data->version);
	if (print_all) {
		printf("%c\toutput-node-id: %u\n", MARK_CHANGE(0), info->output_node_id);
		printf("%c\toutput-port-id: %u\n", MARK_CHANGE(0), info->output_port_id);
		printf("%c\tinput-node-id: %u\n", MARK_CHANGE(1), info->input_node_id);
		printf("%c\tinput-port-id: %u\n", MARK_CHANGE(1), info->input_port_id);
		printf("%c\tstate: \"%s\"", MARK_CHANGE(2), pw_link_state_as_string(info->state));
		if (info->state == PW_LINK_STATE_ERROR && info->error)
			printf(" \"%s\"\n", info->error);
		else
			printf("\n");
		printf("%c\tformat:\n", MARK_CHANGE(3));
		if (info->format)
			spa_debug_format(2, NULL, info->format);
		else
			printf("\t\tnone\n");
		print_properties(info->props, MARK_CHANGE(4));
	}
}

static const struct pw_link_proxy_events link_events = {
	PW_VERSION_LINK_PROXY_EVENTS,
	.info = link_event_info
};

static void print_device(struct proxy_data *data)
{
	struct pw_device_info *info = data->info;
	bool print_all, print_mark;

	print_all = true;
        if (data->first) {
		printf("added:\n");
		print_mark = false;
		data->first = false;
	}
        else {
		printf("changed:\n");
		print_mark = true;
	}

	printf("\tid: %d\n", data->id);
	printf("\tparent_id: %d\n", data->parent_id);
	printf("\tpermissions: %c%c%c\n", data->permissions & PW_PERM_R ? 'r' : '-',
					  data->permissions & PW_PERM_W ? 'w' : '-',
					  data->permissions & PW_PERM_X ? 'x' : '-');
	printf("\ttype: %s (version %d)\n",
			spa_debug_type_find_name(pw_type_info(), data->type), data->version);
	if (print_all) {
		print_params(data, MARK_CHANGE(1));
		print_properties(info->props, MARK_CHANGE(0));
	}
}


static void device_event_info(void *object, const struct pw_device_info *info)
{
        struct proxy_data *data = object;
	struct pw_device_info *old = data->info;
	uint32_t i;

	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if (old != NULL && info->params[i].flags == old->params[i].flags)
				continue;
			remove_params(data, info->params[i].id, 0);
			if (!SPA_FLAG_CHECK(info->params[i].flags, SPA_PARAM_INFO_READ))
				continue;
			pw_device_proxy_enum_params((struct pw_device_proxy*)data->proxy,
					0, info->params[i].id, 0, 0, NULL);
		}
		add_pending(data);
	}

        data->info = pw_device_info_update(data->info, info);

	if (data->pending_seq == 0)
		data->print_func(data);
}

static const struct pw_device_proxy_events device_events = {
	PW_VERSION_DEVICE_PROXY_EVENTS,
	.info = device_event_info,
        .param = event_param
};

static void
destroy_proxy (void *data)
{
        struct proxy_data *pd = data;

	clear_params(pd);
	remove_pending(pd);

        if (pd->info == NULL)
                return;

	if (pd->destroy)
		pd->destroy(pd->info);
        pd->info = NULL;
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = destroy_proxy,
};

static void registry_event_global(void *data, uint32_t id, uint32_t parent_id,
				  uint32_t permissions, uint32_t type, uint32_t version,
				  const struct spa_dict *props)
{
        struct data *d = data;
        struct pw_proxy *proxy;
        uint32_t client_version;
        const void *events;
	struct proxy_data *pd;
	pw_destroy_t destroy;
	print_func_t print_func = NULL;

	switch (type) {
	case PW_TYPE_INTERFACE_Node:
		events = &node_events;
		client_version = PW_VERSION_NODE_PROXY;
		destroy = (pw_destroy_t) pw_node_info_free;
		print_func = print_node;
		break;
	case PW_TYPE_INTERFACE_Port:
		events = &port_events;
		client_version = PW_VERSION_PORT_PROXY;
		destroy = (pw_destroy_t) pw_port_info_free;
		print_func = print_port;
		break;
	case PW_TYPE_INTERFACE_Module:
		events = &module_events;
		client_version = PW_VERSION_MODULE_PROXY;
		destroy = (pw_destroy_t) pw_module_info_free;
		break;
	case PW_TYPE_INTERFACE_Device:
		events = &device_events;
		client_version = PW_VERSION_DEVICE_PROXY;
		destroy = (pw_destroy_t) pw_device_info_free;
		print_func = print_device;
		break;
	case PW_TYPE_INTERFACE_Factory:
		events = &factory_events;
		client_version = PW_VERSION_FACTORY_PROXY;
		destroy = (pw_destroy_t) pw_factory_info_free;
		break;
	case PW_TYPE_INTERFACE_Client:
		events = &client_events;
		client_version = PW_VERSION_CLIENT_PROXY;
		destroy = (pw_destroy_t) pw_client_info_free;
		break;
	case PW_TYPE_INTERFACE_Link:
		events = &link_events;
		client_version = PW_VERSION_LINK_PROXY;
		destroy = (pw_destroy_t) pw_link_info_free;
		break;
	default:
		printf("added:\n");
		printf("\tid: %u\n", id);
		printf("\tparent_id: %d\n", parent_id);
		printf("\tpermissions: %c%c%c\n", permissions & PW_PERM_R ? 'r' : '-',
						  permissions & PW_PERM_W ? 'w' : '-',
						  permissions & PW_PERM_X ? 'x' : '-');
		printf("\ttype: %s (version %d)\n", spa_debug_type_find_name(pw_type_info(), type), version);
		print_properties(props, ' ');
		return;
	}

        proxy = pw_registry_proxy_bind(d->registry_proxy, id, type,
				       client_version,
				       sizeof(struct proxy_data));
        if (proxy == NULL)
                goto no_mem;

	pd = pw_proxy_get_user_data(proxy);
	pd->data = d;
	pd->first = true;
	pd->proxy = proxy;
	pd->id = id;
	pd->parent_id = parent_id;
	pd->permissions = permissions;
	pd->version = version;
	pd->type = type;
	pd->destroy = destroy;
	pd->pending_seq = 0;
	pd->print_func = print_func;
	spa_list_init(&pd->param_list);
        pw_proxy_add_proxy_listener(proxy, &pd->proxy_proxy_listener, events, pd);
        pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);
        return;

      no_mem:
        printf("failed to create proxy");
        return;
}

static void registry_event_global_remove(void *object, uint32_t id)
{
	printf("removed:\n");
	printf("\tid: %u\n", id);
}

static const struct pw_registry_proxy_events registry_events = {
	PW_VERSION_REGISTRY_PROXY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static const struct pw_core_proxy_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = on_core_info,
	.done = on_core_done,
};

static void on_state_changed(void *_data, enum pw_remote_state old,
			     enum pw_remote_state state, const char *error)
{
	struct data *data = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		printf("remote error: %s\n", error);
		pw_main_loop_quit(data->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));

		data->core_proxy = pw_remote_get_core_proxy(data->remote);
		pw_core_proxy_add_listener(data->core_proxy,
					   &data->core_listener,
					   &core_events, data);
		data->registry_proxy = pw_core_proxy_get_registry(data->core_proxy,
								  PW_VERSION_REGISTRY_PROXY, 0);
		pw_registry_proxy_add_listener(data->registry_proxy,
					       &data->registry_listener,
					       &registry_events, data);
		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_state_changed,
};

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	struct pw_properties *props = NULL;

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL)
		return -1;

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.core = pw_core_new(l, NULL, 0);
	if (data.core == NULL)
		return -1;

	if (argc > 1)
		props = pw_properties_new(PW_REMOTE_PROP_REMOTE_NAME, argv[1], NULL);

	data.remote = pw_remote_new(data.core, props, 0);
	if (data.remote == NULL)
		return -1;

	pw_remote_add_listener(data.remote, &data.remote_listener, &remote_events, &data);
	if (pw_remote_connect(data.remote) < 0)
		return -1;

	spa_list_init(&data.pending_list);

	pw_main_loop_run(data.loop);

	pw_remote_destroy(data.remote);
	pw_core_destroy(data.core);
	pw_main_loop_destroy(data.loop);

	return 0;
}
