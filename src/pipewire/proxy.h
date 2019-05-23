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

#ifndef PIPEWIRE_PROXY_H
#define PIPEWIRE_PROXY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/hook.h>

/** \page page_proxy Proxy
 *
 * \section sec_page_proxy_overview Overview
 *
 * The proxy object is a client side representation of a resource
 * that lives on a remote PipeWire instance.
 *
 * It is used to communicate with the remote object.
 *
 * \section sec_page_proxy_core Core proxy
 *
 * A proxy for a remote core object can be obtained by making
 * a remote connection. See \ref pw_page_remote_api
 *
 * A pw_core_proxy can then be retrieved with \ref pw_remote_get_core_proxy
 *
 * Some methods on proxy object allow creation of more proxy objects or
 * create a binding between a local proxy and global resource.
 *
 * \section sec_page_proxy_create Create
 *
 * A client first creates a new proxy object with pw_proxy_new(). A
 * type must be provided for this object.
 *
 * The protocol of the context will usually install an interface to
 * translate method calls and events to the wire format.
 *
 * The creator of the proxy will usually also install an event
 * implementation of the particular object type.
 *
 * \section sec_page_proxy_bind Bind
 *
 * To actually use the proxy object, one needs to create a server
 * side resource for it. This can be done by, for example, binding
 * to a global object or by calling a method that creates and binds
 * to a new remote object. In all cases, the local id is passed to
 * the server and is used to create a resource with the same id.
 *
 * \section sec_page_proxy_methods Methods
 *
 * To call a method on the proxy use the interface methods. Calling
 * any interface method will result in a request to the server to
 * perform the requested action on the corresponding resource.
 *
 * \section sec_page_proxy_events Events
 *
 * Events send from the server to the proxy will be demarshalled by
 * the protocol and will then result in a call to the installed
 * implementation of the proxy.
 *
 * \section sec_page_proxy_destroy Destroy
 *
 * Use pw_proxy_destroy() to destroy the client side object. This
 * is usually done automatically when the server removes the resource
 * associated to the proxy.
 */

/** \class pw_proxy
 *
 * \brief Represents an object on the client side.
 *
 * A pw_proxy acts as a client side proxy to an object existing in a remote
 * pipewire instance. The proxy is responsible for converting interface functions
 * invoked by the client to PipeWire messages. Events will call the handlers
 * set in listener.
 *
 * See \ref page_proxy
 */
struct pw_proxy;

#include <pipewire/protocol.h>

/** Proxy events, use \ref pw_proxy_add_listener */
struct pw_proxy_events {
#define PW_VERSION_PROXY_EVENTS		0
        uint32_t version;

	/** The proxy is destroyed */
        void (*destroy) (void *data);

	/** a reply to a sync method completed */
        void (*done) (void *data, int seq);

	/** an error occured on the proxy */
        void (*error) (void *data, int seq, int res, const char *message);
};

/** Make a new proxy object. The id can be used to bind to a remote object and
  * can be retrieved with \ref pw_proxy_get_id . */
struct pw_proxy *
pw_proxy_new(struct pw_proxy *factory,	/**< factory */
	     uint32_t type,		/**< interface type */
	     size_t user_data_size	/**< size of user data */);

/** Add an event listener to proxy */
void pw_proxy_add_listener(struct pw_proxy *proxy,
			   struct spa_hook *listener,
			   const struct pw_proxy_events *events,
			   void *data);

/** Add a listener for the events received from the remote resource. The
  * events depend on the type of the remote resource. */
void pw_proxy_add_proxy_listener(struct pw_proxy *proxy,	/**< the proxy */
				 struct spa_hook *listener,	/**< listener */
				 const void *events,		/**< proxied events */
				 void *data			/**< data passed to events */);

/** destroy a proxy */
void pw_proxy_destroy(struct pw_proxy *proxy);

/** Get the user_data. The size was given in \ref pw_proxy_new */
void *pw_proxy_get_user_data(struct pw_proxy *proxy);

/** Get the local id of the proxy */
uint32_t pw_proxy_get_id(struct pw_proxy *proxy);

/** Get the protocol used for the proxy */
struct pw_protocol *pw_proxy_get_protocol(struct pw_proxy *proxy);

/** Generate an sync method for a proxy. This will generate a done event
 * with the same seq number of the reply. */
int pw_proxy_sync(struct pw_proxy *proxy, int seq);

/** Generate an error for a proxy */
int pw_proxy_error(struct pw_proxy *proxy, int res, const char *error, ...);

/** Get the listener of proxy */
struct spa_hook_list *pw_proxy_get_proxy_listeners(struct pw_proxy *proxy);

/** Get the marshal functions for the proxy */
const struct pw_protocol_marshal *pw_proxy_get_marshal(struct pw_proxy *proxy);

#define pw_proxy_notify(p,type,event,ver,...)	spa_hook_list_call(pw_proxy_get_proxy_listeners(p),type,event,ver,## __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_PROXY_H */
