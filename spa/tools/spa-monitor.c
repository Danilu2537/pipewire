/* Simple Plugin API
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
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>

#include <spa/support/log-impl.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/monitor/monitor.h>

#include <spa/debug/dict.h>
#include <spa/debug/pod.h>

static SPA_LOG_IMPL(default_log);

struct data {
	struct spa_log *log;
	struct spa_loop main_loop;

	struct spa_support support[3];
	uint32_t n_support;

	unsigned int n_sources;
	struct spa_source sources[16];

	bool rebuild_fds;
	struct pollfd fds[16];
	unsigned int n_fds;
};


static void inspect_item(struct data *data, struct spa_pod *item)
{
	spa_debug_pod(0, NULL, item);
}

static int on_monitor_info(void *_data, const struct spa_dict *info)
{
	spa_debug_dict(0, info);
	return 0;
}

static int on_monitor_event(void *_data, struct spa_event *event)
{
	struct data *data = _data;

	switch (SPA_MONITOR_EVENT_ID(event)) {
	case SPA_MONITOR_EVENT_Added:
		fprintf(stderr, "added:\n");
		inspect_item(data, SPA_POD_CONTENTS(struct spa_event, event));
		break;
	case SPA_MONITOR_EVENT_Removed:
		fprintf(stderr, "removed:\n");
		inspect_item(data, SPA_POD_CONTENTS(struct spa_event, event));
		break;
	case SPA_MONITOR_EVENT_Changed:
		fprintf(stderr, "changed:\n");
		inspect_item(data, SPA_POD_CONTENTS(struct spa_event, event));
		break;
	}
	return 0;
}

static int do_add_source(void *object, struct spa_source *source)
{
	struct data *data = object;

	data->sources[data->n_sources] = *source;
	data->n_sources++;
	data->rebuild_fds = true;

	return 0;
}

static const struct spa_loop_methods impl_loop = {
	SPA_VERSION_LOOP_METHODS,
	.add_source = do_add_source,
};

static const struct spa_monitor_callbacks impl_callbacks = {
	SPA_VERSION_MONITOR_CALLBACKS,
	.info = on_monitor_info,
	.event = on_monitor_event,
};

static void handle_monitor(struct data *data, struct spa_monitor *monitor)
{
	spa_monitor_set_callbacks(monitor, &impl_callbacks, data);

	while (true) {
		int r;
		uint32_t i;

		/* rebuild */
		if (data->rebuild_fds) {
			for (i = 0; i < data->n_sources; i++) {
				struct spa_source *p = &data->sources[i];
				data->fds[i].fd = p->fd;
				data->fds[i].events = p->mask;
			}
			data->n_fds = data->n_sources;
			data->rebuild_fds = false;
		}

		r = poll((struct pollfd *) data->fds, data->n_fds, -1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0) {
			fprintf(stderr, "monitor %p: select timeout", monitor);
			break;
		}

		/* after */
		for (i = 0; i < data->n_sources; i++) {
			struct spa_source *p = &data->sources[i];
			p->func(p);
		}
	}
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	int res;
	void *handle;
	spa_handle_factory_enum_func_t enum_func;
	uint32_t fidx;

	data.log = &default_log.log;
	data.main_loop.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Loop,
			SPA_VERSION_LOOP,
			&impl_loop, &data);

	data.support[1] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, data.log);
	data.support[2] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_MainLoop, &data.main_loop);
	data.n_support = 3;

	if (argc < 2) {
		printf("usage: %s <plugin.so>\n", argv[0]);
		return -1;
	}

	if ((handle = dlopen(argv[1], RTLD_NOW)) == NULL) {
		printf("can't load %s\n", argv[1]);
		return -1;
	}
	if ((enum_func = dlsym(handle, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		printf("can't find function\n");
		return -1;
	}

	for (fidx = 0;;) {
		const struct spa_handle_factory *factory;
		uint32_t iidx;

		if ((res = enum_func(&factory, &fidx)) <= 0) {
			if (res != 0)
				printf("can't enumerate factories: %d\n", res);
			break;
		}

		for (iidx = 0;;) {
			const struct spa_interface_info *info;

			if ((res =
			     spa_handle_factory_enum_interface_info(factory, &info, &iidx)) <= 0) {
				if (res != 0)
					printf("can't enumerate interfaces: %d\n", res);
				break;
			}

			if (info->type == SPA_TYPE_INTERFACE_Monitor) {
				struct spa_handle *handle;
				void *interface;

				handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
				if ((res =
				     spa_handle_factory_init(factory, handle, NULL, data.support,
							     data.n_support)) < 0) {
					printf("can't make factory instance: %s\n", strerror(res));
					continue;
				}

				if ((res =
				     spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Monitor,
							      &interface)) < 0) {
					printf("can't get interface: %s\n", strerror(res));
					continue;
				}
				handle_monitor(&data, interface);
			}
		}
	}

	return 0;
}
