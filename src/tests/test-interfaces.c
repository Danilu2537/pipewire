/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
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

#include <pipewire/pipewire.h>
#include <pipewire/interfaces.h>

#define TEST_FUNC(a,b,func)	\
do {				\
	a.func = b.func;	\
	spa_assert(SPA_PTRDIFF(&a.func, &a) == SPA_PTRDIFF(&b.func, &b)); \
} while(0)

static void test_core_abi(void)
{
	struct pw_core_proxy_methods m;
	struct pw_core_proxy_events e;
	struct {
		uint32_t version;
		int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_core_proxy_events *events,
			void *data);
		int (*hello) (void *object, uint32_t version);
		int (*sync) (void *object, uint32_t id, int seq);
		int (*pong) (void *object, uint32_t id, int seq);
		int (*error) (void *object, uint32_t id, int seq, int res, const char *error);
		struct pw_registry_proxy * (*get_registry) (void *object,
				uint32_t version, size_t user_data_size);
		void * (*create_object) (void *object,
				       const char *factory_name,
				       uint32_t type,
				       uint32_t version,
				       const struct spa_dict *props,
				       size_t user_data_size);
		int (*destroy) (void *object, void *proxy);
	} methods = { PW_VERSION_CORE_PROXY_METHODS, };
	struct {
		uint32_t version;
		void (*info) (void *object, const struct pw_core_info *info);
		void (*done) (void *object, uint32_t id, int seq);
		void (*ping) (void *object, uint32_t id, int seq);
		void (*error) (void *object, uint32_t id, int seq, int res, const char *error);
		void (*remove_id) (void *object, uint32_t id);
	} events = { PW_VERSION_CORE_PROXY_EVENTS, };

	TEST_FUNC(m, methods, version);
	TEST_FUNC(m, methods, add_listener);
	TEST_FUNC(m, methods, hello);
	TEST_FUNC(m, methods, sync);
	TEST_FUNC(m, methods, pong);
	TEST_FUNC(m, methods, error);
	TEST_FUNC(m, methods, get_registry);
	TEST_FUNC(m, methods, create_object);
	TEST_FUNC(m, methods, destroy);
	spa_assert(PW_VERSION_CORE_PROXY_METHODS == 0);
	spa_assert(sizeof(m) == sizeof(methods));

	TEST_FUNC(e, events, version);
	TEST_FUNC(e, events, info);
	TEST_FUNC(e, events, done);
	TEST_FUNC(e, events, ping);
	TEST_FUNC(e, events, error);
	TEST_FUNC(e, events, remove_id);
	spa_assert(PW_VERSION_CORE_PROXY_EVENTS == 0);
	spa_assert(sizeof(e) == sizeof(events));
}

static void test_registry_abi(void)
{
	struct pw_registry_proxy_methods m;
	struct pw_registry_proxy_events e;
	struct {
		uint32_t version;
		int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_registry_proxy_events *events,
			void *data);
		void * (*bind) (void *object, uint32_t id, uint32_t type, uint32_t version,
				size_t user_data_size);
		int (*destroy) (void *object, uint32_t id);
	} methods = { PW_VERSION_REGISTRY_PROXY_METHODS, };
	struct {
		uint32_t version;
		void (*global) (void *object, uint32_t id, uint32_t parent_id,
			uint32_t permissions, uint32_t type, uint32_t version,
			const struct spa_dict *props);
		void (*global_remove) (void *object, uint32_t id);
	} events = { PW_VERSION_REGISTRY_PROXY_EVENTS, };

	TEST_FUNC(m, methods, version);
	TEST_FUNC(m, methods, add_listener);
	TEST_FUNC(m, methods, bind);
	TEST_FUNC(m, methods, destroy);
	spa_assert(PW_VERSION_REGISTRY_PROXY_METHODS == 0);
	spa_assert(sizeof(m) == sizeof(methods));

	TEST_FUNC(e, events, version);
	TEST_FUNC(e, events, global);
	TEST_FUNC(e, events, global_remove);
	spa_assert(PW_VERSION_REGISTRY_PROXY_EVENTS == 0);
	spa_assert(sizeof(e) == sizeof(events));
}

static void test_module_abi(void)
{
	struct pw_module_proxy_methods m;
	struct pw_module_proxy_events e;
	struct {
		uint32_t version;
		int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_module_proxy_events *events,
			void *data);
	} methods = { PW_VERSION_MODULE_PROXY_METHODS, };
	struct {
		uint32_t version;
		void (*info) (void *object, const struct pw_module_info *info);
	} events = { PW_VERSION_MODULE_PROXY_EVENTS, };

	TEST_FUNC(m, methods, version);
	TEST_FUNC(m, methods, add_listener);
	spa_assert(PW_VERSION_MODULE_PROXY_METHODS == 0);
	spa_assert(sizeof(m) == sizeof(methods));

	TEST_FUNC(e, events, version);
	TEST_FUNC(e, events, info);
	spa_assert(PW_VERSION_MODULE_PROXY_EVENTS == 0);
	spa_assert(sizeof(e) == sizeof(events));
}

static void test_device_abi(void)
{
	struct pw_device_proxy_methods m;
	struct pw_device_proxy_events e;
	struct {
		uint32_t version;
		int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_device_proxy_events *events,
			void *data);
		int (*enum_params) (void *object, int seq, uint32_t id,
			uint32_t start, uint32_t num,
			const struct spa_pod *filter);
		int (*set_param) (void *object, uint32_t id, uint32_t flags,
			const struct spa_pod *param);
	} methods = { PW_VERSION_DEVICE_PROXY_METHODS, };
	struct {
		uint32_t version;
		void (*info) (void *object, const struct pw_device_info *info);
		void (*param) (void *object, int seq,
			uint32_t id, uint32_t index, uint32_t next,
			const struct spa_pod *param);
	} events = { PW_VERSION_DEVICE_PROXY_EVENTS, };

	TEST_FUNC(m, methods, version);
	TEST_FUNC(m, methods, add_listener);
	TEST_FUNC(m, methods, enum_params);
	TEST_FUNC(m, methods, set_param);
	spa_assert(PW_VERSION_DEVICE_PROXY_METHODS == 0);
	spa_assert(sizeof(m) == sizeof(methods));

	TEST_FUNC(e, events, version);
	TEST_FUNC(e, events, info);
	TEST_FUNC(e, events, param);
	spa_assert(PW_VERSION_DEVICE_PROXY_EVENTS == 0);
	spa_assert(sizeof(e) == sizeof(events));
}

static void test_node_abi(void)
{
	struct pw_node_proxy_methods m;
	struct pw_node_proxy_events e;
	struct {
		uint32_t version;
		int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_node_proxy_events *events,
			void *data);
		int (*subscribe_params) (void *object, uint32_t *ids, uint32_t n_ids);
		int (*enum_params) (void *object, int seq, uint32_t id,
			uint32_t start, uint32_t num, const struct spa_pod *filter);
		int (*set_param) (void *object, uint32_t id, uint32_t flags,
			const struct spa_pod *param);
		int (*send_command) (void *object, const struct spa_command *command);
	} methods = { PW_VERSION_NODE_PROXY_METHODS, };
	struct {
		uint32_t version;
		void (*info) (void *object, const struct pw_node_info *info);
		void (*param) (void *object, int seq,
			uint32_t id, uint32_t index, uint32_t next,
			const struct spa_pod *param);
	} events = { PW_VERSION_NODE_PROXY_EVENTS, };

	TEST_FUNC(m, methods, version);
	TEST_FUNC(m, methods, add_listener);
	TEST_FUNC(m, methods, subscribe_params);
	TEST_FUNC(m, methods, enum_params);
	TEST_FUNC(m, methods, set_param);
	TEST_FUNC(m, methods, send_command);
	spa_assert(PW_VERSION_NODE_PROXY_METHODS == 0);
	spa_assert(sizeof(m) == sizeof(methods));

	TEST_FUNC(e, events, version);
	TEST_FUNC(e, events, info);
	TEST_FUNC(e, events, param);
	spa_assert(PW_VERSION_NODE_PROXY_EVENTS == 0);
	spa_assert(sizeof(e) == sizeof(events));
}

static void test_port_abi(void)
{
	struct pw_port_proxy_methods m;
	struct pw_port_proxy_events e;
	struct {
		uint32_t version;
		int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_port_proxy_events *events,
			void *data);
		int (*subscribe_params) (void *object, uint32_t *ids, uint32_t n_ids);
		int (*enum_params) (void *object, int seq, uint32_t id,
			uint32_t start, uint32_t num, const struct spa_pod *filter);
	} methods = { PW_VERSION_PORT_PROXY_METHODS, };
	struct {
		uint32_t version;
		void (*info) (void *object, const struct pw_port_info *info);
		void (*param) (void *object, int seq,
			uint32_t id, uint32_t index, uint32_t next,
			const struct spa_pod *param);
	} events = { PW_VERSION_PORT_PROXY_EVENTS, };

	TEST_FUNC(m, methods, version);
	TEST_FUNC(m, methods, add_listener);
	TEST_FUNC(m, methods, enum_params);
	spa_assert(PW_VERSION_PORT_PROXY_METHODS == 0);
	spa_assert(sizeof(m) == sizeof(methods));

	TEST_FUNC(e, events, version);
	TEST_FUNC(e, events, info);
	TEST_FUNC(e, events, param);
	spa_assert(PW_VERSION_PORT_PROXY_EVENTS == 0);
	spa_assert(sizeof(e) == sizeof(events));
}

static void test_factory_abi(void)
{
	struct pw_factory_proxy_methods m;
	struct pw_factory_proxy_events e;
	struct {
		uint32_t version;
		int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_factory_proxy_events *events,
			void *data);
	} methods = { PW_VERSION_FACTORY_PROXY_METHODS, };
	struct {
		uint32_t version;
		void (*info) (void *object, const struct pw_factory_info *info);
	} events = { PW_VERSION_FACTORY_PROXY_EVENTS, };

	TEST_FUNC(m, methods, version);
	TEST_FUNC(m, methods, add_listener);
	spa_assert(PW_VERSION_FACTORY_PROXY_METHODS == 0);
	spa_assert(sizeof(m) == sizeof(methods));

	TEST_FUNC(e, events, version);
	TEST_FUNC(e, events, info);
	spa_assert(PW_VERSION_FACTORY_PROXY_EVENTS == 0);
	spa_assert(sizeof(e) == sizeof(events));
}

static void test_client_abi(void)
{
	struct pw_client_proxy_methods m;
	struct pw_client_proxy_events e;
	struct {
		uint32_t version;
		int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_client_proxy_events *events,
			void *data);
		int (*error) (void *object, uint32_t id, int res, const char *error);
		int (*update_properties) (void *object, const struct spa_dict *props);
		int (*get_permissions) (void *object, uint32_t index, uint32_t num);
		int (*update_permissions) (void *object, uint32_t n_permissions,
			const struct pw_permission *permissions);
	} methods = { PW_VERSION_CLIENT_PROXY_METHODS, };
	struct {
		uint32_t version;
		void (*info) (void *object, const struct pw_client_info *info);
		void (*permissions) (void *object, uint32_t index,
			uint32_t n_permissions, const struct pw_permission *permissions);
	} events = { PW_VERSION_CLIENT_PROXY_EVENTS, };

	TEST_FUNC(m, methods, version);
	TEST_FUNC(m, methods, add_listener);
	TEST_FUNC(m, methods, error);
	TEST_FUNC(m, methods, update_properties);
	TEST_FUNC(m, methods, get_permissions);
	TEST_FUNC(m, methods, update_permissions);
	spa_assert(PW_VERSION_CLIENT_PROXY_METHODS == 0);
	spa_assert(sizeof(m) == sizeof(methods));

	TEST_FUNC(e, events, version);
	TEST_FUNC(e, events, info);
	TEST_FUNC(e, events, permissions);
	spa_assert(PW_VERSION_CLIENT_PROXY_EVENTS == 0);
	spa_assert(sizeof(e) == sizeof(events));
}

static void test_link_abi(void)
{
	struct pw_link_proxy_methods m;
	struct pw_link_proxy_events e;
	struct {
		uint32_t version;
		int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_link_proxy_events *events,
			void *data);
	} methods = { PW_VERSION_LINK_PROXY_METHODS, };
	struct {
		uint32_t version;
		void (*info) (void *object, const struct pw_link_info *info);
	} events = { PW_VERSION_LINK_PROXY_EVENTS, };

	TEST_FUNC(m, methods, version);
	TEST_FUNC(m, methods, add_listener);
	spa_assert(PW_VERSION_LINK_PROXY_METHODS == 0);
	spa_assert(sizeof(m) == sizeof(methods));

	TEST_FUNC(e, events, version);
	TEST_FUNC(e, events, info);
	spa_assert(PW_VERSION_LINK_PROXY_EVENTS == 0);
	spa_assert(sizeof(e) == sizeof(events));
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);

	test_core_abi();
	test_registry_abi();
	test_module_abi();
	test_device_abi();
	test_node_abi();
	test_port_abi();
	test_factory_abi();
	test_client_abi();
	test_link_abi();

	return 0;
}
