/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

#include <spa/node/node.h>
#include <spa/lib/pod.h>

#include "pipewire/pipewire.h"
#include "pipewire/interfaces.h"
#include "pipewire/private.h"

#include "pipewire/core.h"
#include "modules/spa/spa-node.h"
#include "client-node.h"

/** \cond */

#define MAX_INPUTS	64
#define MAX_OUTPUTS	64

#define MAX_BUFFERS	64
#define MAX_AREAS	1024
#define MAX_IO		32

#define CHECK_IN_PORT_ID(this,d,p)       ((d) == SPA_DIRECTION_INPUT && (p) < MAX_INPUTS)
#define CHECK_OUT_PORT_ID(this,d,p)      ((d) == SPA_DIRECTION_OUTPUT && (p) < MAX_OUTPUTS)
#define CHECK_PORT_ID(this,d,p)          (CHECK_IN_PORT_ID(this,d,p) || CHECK_OUT_PORT_ID(this,d,p))
#define CHECK_FREE_IN_PORT(this,d,p)     (CHECK_IN_PORT_ID(this,d,p) && !(this)->in_ports[p].valid)
#define CHECK_FREE_OUT_PORT(this,d,p)    (CHECK_OUT_PORT_ID(this,d,p) && !(this)->out_ports[p].valid)
#define CHECK_FREE_PORT(this,d,p)        (CHECK_FREE_IN_PORT (this,d,p) || CHECK_FREE_OUT_PORT (this,d,p))
#define CHECK_IN_PORT(this,d,p)          (CHECK_IN_PORT_ID(this,d,p) && (this)->in_ports[p].valid)
#define CHECK_OUT_PORT(this,d,p)         (CHECK_OUT_PORT_ID(this,d,p) && (this)->out_ports[p].valid)
#define CHECK_PORT(this,d,p)             (CHECK_IN_PORT (this,d,p) || CHECK_OUT_PORT (this,d,p))

#define GET_IN_PORT(this,p)	(&this->in_ports[p])
#define GET_OUT_PORT(this,p)	(&this->out_ports[p])
#define GET_PORT(this,d,p)	(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))

#define CHECK_PORT_BUFFER(this,b,p)      (b < p->n_buffers)

struct mem {
	uint32_t id;
	int ref;
	int fd;
	uint32_t type;
	uint32_t flags;
};

struct buffer {
	struct spa_buffer *outbuf;
	struct spa_buffer buffer;
	struct spa_meta metas[4];
	struct spa_data datas[4];
	bool outstanding;
	uint32_t memid;
};

struct io {
	uint32_t id;
	uint32_t memid;
	uint32_t mix_id;
};

struct port {
	bool valid;
	struct spa_port_info info;
	struct pw_properties *properties;

	bool have_format;
	uint32_t n_params;
	struct spa_pod **params;

	uint32_t n_buffers;
	struct buffer buffers[MAX_BUFFERS];

	struct io ios[MAX_IO];
};

struct node {
	struct spa_node node;

	struct impl *impl;

	struct spa_type_map *map;
	struct spa_log *log;
	struct spa_loop *data_loop;

	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;

	struct pw_resource *resource;

	struct spa_source data_source;
	int writefd;

	uint32_t max_inputs;
	uint32_t n_inputs;
	uint32_t max_outputs;
	uint32_t n_outputs;
	struct port in_ports[MAX_INPUTS];
	struct port out_ports[MAX_OUTPUTS];

	uint32_t n_params;
	struct spa_pod **params;

	uint32_t seq;
};

struct impl {
	struct pw_client_node this;

	struct pw_core *core;
	struct pw_type *t;

	struct node node;

	struct pw_map io_map;
	struct pw_memblock *io_areas;

	struct spa_hook node_listener;
	struct spa_hook resource_listener;

	struct pw_array mems;

	int fds[2];
	int other_fds[2];

	struct pw_client_node_position *position;
	uint32_t next_position;
};

/** \endcond */

static struct mem *ensure_mem(struct impl *impl, int fd, uint32_t type, uint32_t flags)
{
	struct mem *m, *f = NULL;

	pw_array_for_each(m, &impl->mems) {
		if (m->ref <= 0)
			f = m;
		else if (m->fd == fd)
			goto found;
	}

	if (f == NULL) {
		m = pw_array_add(&impl->mems, sizeof(struct mem));
		m->id = pw_array_get_len(&impl->mems, struct mem) - 1;
		m->ref = 0;
	}
	else {
		m = f;
	}
	m->fd = fd;
	m->type = type;
	m->flags = flags;

	pw_client_node_resource_add_mem(impl->node.resource,
					m->id,
					type,
					m->fd,
					m->flags);
      found:
	m->ref++;
	return m;
}

static void clear_io(struct node *node, struct io *io)
{
	struct mem *m;
	m = pw_array_get_unchecked(&node->impl->mems, io->memid, struct mem);
	m->ref--;
	io->id = SPA_ID_INVALID;
}

static struct io *update_io(struct impl *impl, struct port *port,
		uint32_t mix_id, uint32_t id, uint32_t memid)
{
	int i;
	struct io *io, *f = NULL;

	for (i = 0; i < MAX_IO; i++) {
		io = &port->ios[i];
		if (io->id == SPA_ID_INVALID)
			f = io;
		else if (io->id == id && io->mix_id == mix_id) {
			if (io->memid != memid) {
				clear_io(&impl->node, io);
				if (memid == SPA_ID_INVALID)
					io->id = SPA_ID_INVALID;
			}
			goto found;
		}
	}
	if (f == NULL)
		return NULL;

	io = f;
	io->id = id;
	io->memid = memid;
	io->mix_id = mix_id;

     found:
	return io;
}

static void clear_ios(struct node *this, struct port *port)
{
	int i;

	for (i = 0; i < MAX_IO; i++) {
		struct io *io = &port->ios[i];
		if (io->id != SPA_ID_INVALID)
			clear_io(this, io);
	}
}

static int clear_buffers(struct node *this, struct port *port)
{
	uint32_t i, j;
	struct impl *impl = this->impl;
	struct pw_type *t = impl->t;

	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b = &port->buffers[i];
		struct mem *m;

		spa_log_debug(this->log, "node %p: clear buffer %d", this, i);

		for (j = 0; j < b->buffer.n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			if (d->type == t->data.DmaBuf ||
			    d->type == t->data.MemFd) {
				uint32_t id;

				id = SPA_PTR_TO_UINT32(b->buffer.datas[j].data);
				m = pw_array_get_unchecked(&impl->mems, id, struct mem);
				m->ref--;
			}
		}
		m = pw_array_get_unchecked(&impl->mems, b->memid, struct mem);
		m->ref--;
	}
	port->n_buffers = 0;
	return 0;
}

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **result,
				 struct spa_pod_builder *builder)
{
	struct node *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	while (true) {
		struct spa_pod *param;

		if (*index >= this->n_params)
			return 0;

		param = this->params[(*index)++];

		if (!spa_pod_is_object_id(param, id))
			continue;

		if (spa_pod_filter(builder, result, param, filter) == 0)
			break;
	}
	return 1;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct node *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (this->resource == NULL)
		return 0;

	pw_client_node_resource_set_param(this->resource, this->seq, id, flags, param);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct node *this;
	int res = 0;
	struct pw_type *t;

	if (node == NULL || command == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (this->resource == NULL)
		return 0;

	t = this->impl->t;

	if (SPA_COMMAND_TYPE(command) == t->command_node.ClockUpdate) {
		pw_client_node_resource_command(this->resource, this->seq++, command);
	} else {
		/* send start */
		pw_client_node_resource_command(this->resource, this->seq, command);
		res = SPA_RESULT_RETURN_ASYNC(this->seq++);
	}
	return res;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct node *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct node, node);
	this->callbacks = callbacks;
	this->callbacks_data = data;

	return 0;
}

static int
impl_node_get_n_ports(struct spa_node *node,
		      uint32_t *n_input_ports,
		      uint32_t *max_input_ports,
		      uint32_t *n_output_ports,
		      uint32_t *max_output_ports)
{
	struct node *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (n_input_ports)
		*n_input_ports = this->n_inputs;
	if (max_input_ports)
		*max_input_ports = this->max_inputs == 0 ? this->n_inputs : this->max_inputs;
	if (n_output_ports)
		*n_output_ports = this->n_outputs;
	if (max_output_ports)
		*max_output_ports = this->max_outputs == 0 ? this->n_outputs : this->max_outputs;

	return 0;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t *input_ids,
		       uint32_t n_input_ids,
		       uint32_t *output_ids,
		       uint32_t n_output_ids)
{
	struct node *this;
	int c, i;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (input_ids) {
		for (c = 0, i = 0; i < MAX_INPUTS && c < n_input_ids; i++) {
			if (this->in_ports[i].valid)
				input_ids[c++] = i;
		}
	}
	if (output_ids) {
		for (c = 0, i = 0; i < MAX_OUTPUTS && c < n_output_ids; i++) {
			if (this->out_ports[i].valid)
				output_ids[c++] = i;
		}
	}
	return 0;
}

static void
do_update_port(struct node *this,
	       enum spa_direction direction,
	       uint32_t port_id,
	       uint32_t change_mask,
	       uint32_t n_params,
	       const struct spa_pod **params,
	       const struct spa_port_info *info)
{
	struct port *port;
	struct pw_type *t = this->impl->t;
	int i;

	port = GET_PORT(this, direction, port_id);

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_PARAMS) {
		port->have_format = false;

		spa_log_debug(this->log, "node %p: port %u update %d params", this, port_id, n_params);
		for (i = 0; i < port->n_params; i++)
			free(port->params[i]);
		port->n_params = n_params;
		port->params = realloc(port->params, port->n_params * sizeof(struct spa_pod *));

		for (i = 0; i < port->n_params; i++) {
			port->params[i] = pw_spa_pod_copy(params[i]);

			if (spa_pod_is_object_id(port->params[i], t->param.idFormat))
				port->have_format = true;
		}
	}

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_INFO) {
		if (port->properties)
			pw_properties_free(port->properties);
		port->properties = NULL;
		port->info.props = NULL;

		if (info) {
			port->info = *info;
			if (info->props) {
				port->properties = pw_properties_new_dict(info->props);
				port->info.props = &port->properties->dict;
			}
		}
	}

	if (!port->valid) {
		spa_log_debug(this->log, "node %p: adding port %d", this, port_id);
		port->have_format = false;
		port->valid = true;
		for (i = 0; i < MAX_IO; i++)
			port->ios[i].id = SPA_ID_INVALID;

		if (direction == SPA_DIRECTION_INPUT)
			this->n_inputs++;
		else
			this->n_outputs++;
	}
}

static void
clear_port(struct node *this,
	   struct port *port, enum spa_direction direction, uint32_t port_id)
{
	do_update_port(this,
		       direction,
		       port_id,
		       PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
		       PW_CLIENT_NODE_PORT_UPDATE_INFO, 0, NULL, NULL);
	clear_buffers(this, port);
	clear_ios(this, port);
}

static void do_uninit_port(struct node *this, enum spa_direction direction, uint32_t port_id)
{
	struct port *port;

	spa_log_debug(this->log, "node %p: removing port %d", this, port_id);

	if (direction == SPA_DIRECTION_INPUT) {
		port = GET_IN_PORT(this, port_id);
		this->n_inputs--;
	} else {
		port = GET_OUT_PORT(this, port_id);
		this->n_outputs--;
	}
	clear_port(this, port, direction, port_id);
	port->valid = false;
}

static int
impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct node *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (!CHECK_FREE_PORT(this, direction, port_id))
		return -EINVAL;

	pw_client_node_resource_add_port(this->resource,
					 this->seq,
					 direction, port_id);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct node *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (!CHECK_PORT(this, direction, port_id))
		return -EINVAL;

	pw_client_node_resource_remove_port(this->resource,
					    this->seq,
					    direction, port_id);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction, uint32_t port_id,
			const struct spa_port_info **info)
{
	struct node *this;
	struct port *port;

	if (node == NULL || info == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (!CHECK_PORT(this, direction, port_id))
		return -EINVAL;

	port = GET_PORT(this, direction, port_id);
	*info = &port->info;

	return 0;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{
	struct node *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	while (true) {
		struct spa_pod *param;

		if (*index >= port->n_params)
			return 0;

		param = port->params[(*index)++];

		if (!spa_pod_is_object_id(param, id))
			continue;

		if (spa_pod_filter(builder, result, param, filter) == 0)
			break;
	}
	return 1;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct node *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (!CHECK_PORT(this, direction, port_id))
		return -EINVAL;

	if (this->resource == NULL)
		return 0;

	pw_client_node_resource_port_set_param(this->resource,
					       this->seq,
					       direction, port_id,
					       id, flags,
					       param);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int do_port_set_io(struct impl *impl,
			   enum spa_direction direction, uint32_t port_id,
			   struct pw_port_mix *mix,
			   uint32_t id, void *data, size_t size)
{
	struct node *this = &impl->node;
	struct pw_type *t = impl->t;
	struct pw_memblock *mem;
	struct mem *m;
	uint32_t memid, mem_offset, mem_size;
	struct port *port;

	pw_log_debug("client-node %p: %s port %d.%d set io %p %zd", impl,
			direction == SPA_DIRECTION_INPUT ? "input" : "output",
			port_id, mix->port.port_id, data, size);

	if (!CHECK_PORT(this, direction, port_id))
		return -EINVAL;

	if (this->resource == NULL)
		return 0;

	port = GET_PORT(this, direction, port_id);

	if (data) {
		if ((mem = pw_memblock_find(data)) == NULL)
			return -EINVAL;

		mem_offset = SPA_PTRDIFF(data, mem->ptr);
		mem_size = mem->size;
		if (mem_size - mem_offset < size)
			return -EINVAL;

		mem_offset += mem->offset;
		m = ensure_mem(impl, mem->fd, t->data.MemFd, mem->flags);
		memid = m->id;
	}
	else {
		memid = SPA_ID_INVALID;
		mem_offset = mem_size = 0;
	}
	mix->io = data;
	update_io(impl, port, mix->port.port_id, id, memid);

	pw_client_node_resource_port_set_io(this->resource,
					    this->seq,
					    direction, port_id,
					    mix->port.port_id,
					    id,
					    memid,
					    mem_offset, mem_size);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	return -ENOTSUP;
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct node *this;
	struct impl *impl;
	struct port *port;
	uint32_t i, j;
	struct pw_client_node_buffer *mb;
	struct pw_type *t;

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;
	spa_log_debug(this->log, "node %p: use buffers %p %u", this, buffers, n_buffers);

	t = impl->t;

	if (!CHECK_PORT(this, direction, port_id))
		return -EINVAL;

	port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return -EIO;

	clear_buffers(this, port);

	if (n_buffers > 0) {
		mb = alloca(n_buffers * sizeof(struct pw_client_node_buffer));
	} else {
		mb = NULL;
	}

	port->n_buffers = n_buffers;

	if (this->resource == NULL)
		return 0;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &port->buffers[i];
		struct pw_memblock *mem;
		struct mem *m;
		size_t data_size, size;
		void *baseptr;

		b->outbuf = buffers[i];
		memcpy(&b->buffer, buffers[i], sizeof(struct spa_buffer));
		b->buffer.datas = b->datas;
		b->buffer.metas = b->metas;

		if (buffers[i]->n_metas > 0)
			baseptr = buffers[i]->metas[0].data;
		else if (buffers[i]->n_datas > 0)
			baseptr = buffers[i]->datas[0].chunk;
		else
			return -EINVAL;

		if ((mem = pw_memblock_find(baseptr)) == NULL)
			return -EINVAL;

		data_size = 0;
		for (j = 0; j < buffers[i]->n_metas; j++) {
			data_size += buffers[i]->metas[j].size;
		}
		for (j = 0; j < buffers[i]->n_datas; j++) {
			struct spa_data *d = buffers[i]->datas;
			data_size += sizeof(struct spa_chunk);
			if (d->type == t->data.MemPtr)
				data_size += d->maxsize;
		}

		m = ensure_mem(impl, mem->fd, t->data.MemFd, mem->flags);
		b->memid = m->id;

		mb[i].buffer = &b->buffer;
		mb[i].mem_id = b->memid;
		mb[i].offset = SPA_PTRDIFF(baseptr, mem->ptr + mem->offset);
		mb[i].size = data_size;

		for (j = 0; j < buffers[i]->n_metas; j++)
			memcpy(&b->buffer.metas[j], &buffers[i]->metas[j], sizeof(struct spa_meta));
		b->buffer.n_metas = j;

		size = buffers[i]->n_datas * sizeof(struct spa_chunk);
		for (j = 0; j < buffers[i]->n_datas; j++) {
			struct spa_data *d = &buffers[i]->datas[j];

			memcpy(&b->buffer.datas[j], d, sizeof(struct spa_data));

			if (d->type == t->data.DmaBuf ||
			    d->type == t->data.MemFd) {
				m = ensure_mem(impl, d->fd, d->type, d->flags);
				b->buffer.datas[j].data = SPA_UINT32_TO_PTR(m->id);
			} else if (d->type == t->data.MemPtr) {
				b->buffer.datas[j].data = SPA_INT_TO_PTR(size);
				size += d->maxsize;
			} else {
				b->buffer.datas[j].type = SPA_ID_INVALID;
				b->buffer.datas[j].data = NULL;
				spa_log_error(this->log, "invalid memory type %d", d->type);
			}
		}
	}

	pw_client_node_resource_port_use_buffers(this->resource,
						 this->seq,
						 direction, port_id, 0,
						 n_buffers, mb);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_pod **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	struct node *this;
	struct port *port;

	if (node == NULL || buffers == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (!CHECK_PORT(this, direction, port_id))
		return -EINVAL;

	port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return -EIO;

	return -ENOTSUP;
}

static int
impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct node *this;

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (!CHECK_OUT_PORT(this, SPA_DIRECTION_OUTPUT, port_id))
		return -EINVAL;

	spa_log_trace(this->log, "reuse buffer %d", buffer_id);

	return -ENOTSUP;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id, const struct spa_command *command)
{
	struct node *this;
	struct impl *impl;
	struct pw_type *t;

	if (node == NULL || command == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (this->resource == NULL)
		return 0;

	impl = this->impl;
	t = impl->t;

	spa_log_trace(this->log, "send command %s",
			spa_type_map_get_type(t->map, SPA_COMMAND_TYPE(command)));

	pw_client_node_resource_port_command(this->resource,
					     direction, port_id,
					     command);
	return 0;
}

static int impl_node_process(struct spa_node *node)
{
	struct node *this = SPA_CONTAINER_OF(node, struct node, node);
	struct impl *impl = this->impl;
	struct pw_driver_quantum *q;
	uint64_t cmd = 1;

	if (this->impl->this.status != SPA_ID_INVALID) {
		spa_log_trace(this->log, "%p: return %d", this, this->impl->this.status);
		return impl->this.status;
	}

	spa_log_trace(this->log, "%p: send process %p", this, impl->this.node->driver_node);

	q = impl->this.node->driver_node->rt.quantum;

	impl->position->nsec = q->time;
	impl->position->duration = q->size;
	impl->position->position = impl->next_position;
	impl->position->rate = q->rate;
	impl->next_position += q->size;

	if (write(this->writefd, &cmd, 8) != 8)
		spa_log_warn(this->log, "node %p: error %s", this, strerror(errno));

	return SPA_STATUS_OK;
}

static void
client_node_done(void *data, int seq, int res)
{
	struct impl *impl = data;
	struct node *this = &impl->node;

	this->callbacks->done(this->callbacks_data, seq, res);
}

static void
client_node_update(void *data,
		   uint32_t change_mask,
		   uint32_t max_input_ports,
		   uint32_t max_output_ports,
		   uint32_t n_params,
		   const struct spa_pod **params)
{
	struct impl *impl = data;
	struct node *this = &impl->node;

	if (change_mask & PW_CLIENT_NODE_UPDATE_MAX_INPUTS)
		this->max_inputs = max_input_ports;
	if (change_mask & PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS)
		this->max_outputs = max_output_ports;
	if (change_mask & PW_CLIENT_NODE_UPDATE_PARAMS) {
		int i;
		spa_log_debug(this->log, "node %p: update %d params", this, n_params);

		for (i = 0; i < this->n_params; i++)
			free(this->params[i]);
		this->n_params = n_params;
		this->params = realloc(this->params, this->n_params * sizeof(struct spa_pod *));

		for (i = 0; i < this->n_params; i++)
			this->params[i] = pw_spa_pod_copy(params[i]);
	}
	spa_log_debug(this->log, "node %p: got node update max_in %u, max_out %u", this,
		     this->max_inputs, this->max_outputs);
}

static void
client_node_port_update(void *data,
			enum spa_direction direction,
			uint32_t port_id,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct spa_port_info *info)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	bool remove;

	spa_log_debug(this->log, "node %p: got port update", this);
	if (!CHECK_PORT_ID(this, direction, port_id))
		return;

	remove = (change_mask == 0);

	if (remove) {
		do_uninit_port(this, direction, port_id);
	} else {
		do_update_port(this,
			       direction,
			       port_id,
			       change_mask,
			       n_params, params, info);
	}
	pw_node_update_ports(impl->this.node);
}

static void client_node_set_active(void *data, bool active)
{
	struct impl *impl = data;
	pw_node_set_active(impl->this.node, active);
}

static void client_node_event(void *data, struct spa_event *event)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	this->callbacks->event(this->callbacks_data, event);
}

static void client_node_destroy(void *data)
{
	struct impl *impl = data;
	pw_client_node_destroy(&impl->this);
}

static struct pw_client_node_proxy_methods client_node_methods = {
	PW_VERSION_CLIENT_NODE_PROXY_METHODS,
	.done = client_node_done,
	.update = client_node_update,
	.port_update = client_node_port_update,
	.set_active = client_node_set_active,
	.event = client_node_event,
	.destroy = client_node_destroy,
};

static void node_on_data_fd_events(struct spa_source *source)
{
	struct node *this = source->data;

	if (source->rmask & (SPA_IO_ERR | SPA_IO_HUP)) {
		spa_log_warn(this->log, "node %p: got error", this);
		return;
	}

	if (source->rmask & SPA_IO_IN) {
		uint64_t cmd;

		if (read(this->data_source.fd, &cmd, sizeof(uint64_t)) != sizeof(uint64_t))
			spa_log_warn(this->log, "node %p: error reading message: %s",
					this, strerror(errno));

		spa_log_trace(this->log, "node %p: got process", this);
		this->callbacks->process(this->callbacks_data, SPA_STATUS_HAVE_BUFFER);
	}
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	NULL,
	impl_node_enum_params,
	impl_node_set_param,
	impl_node_send_command,
	impl_node_set_callbacks,
	impl_node_get_n_ports,
	impl_node_get_port_ids,
	impl_node_add_port,
	impl_node_remove_port,
	impl_node_port_get_info,
	impl_node_port_enum_params,
	impl_node_port_set_param,
	impl_node_port_use_buffers,
	impl_node_port_alloc_buffers,
	impl_node_port_set_io,
	impl_node_port_reuse_buffer,
	impl_node_port_send_command,
	impl_node_process,
};

static int
node_init(struct node *this,
	  struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	uint32_t i;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE_LOOP__DataLoop) == 0)
			this->data_loop = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
	}
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data-loop is needed");
		return -EINVAL;
	}
	if (this->map == NULL) {
		spa_log_error(this->log, "a type map is needed");
		return -EINVAL;
	}

	this->node = impl_node;

	this->data_source.func = node_on_data_fd_events;
	this->data_source.data = this;
	this->data_source.fd = -1;
	this->data_source.mask = SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP;
	this->data_source.rmask = 0;

	this->seq = 1;

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int node_clear(struct node *this)
{
	uint32_t i;

	for (i = 0; i < this->n_params; i++)
		free(this->params[i]);
	free(this->params);

	for (i = 0; i < MAX_INPUTS; i++) {
		if (this->in_ports[i].valid)
			clear_port(this, &this->in_ports[i], SPA_DIRECTION_INPUT, i);
	}
	for (i = 0; i < MAX_OUTPUTS; i++) {
		if (this->out_ports[i].valid)
			clear_port(this, &this->out_ports[i], SPA_DIRECTION_OUTPUT, i);
	}

	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct spa_source *source = user_data;
	spa_loop_remove_source(loop, source);
	return 0;
}

static void client_node_resource_destroy(void *data)
{
	struct impl *impl = data;
	struct pw_client_node *this = &impl->this;
	struct node *node = &impl->node;

	pw_log_debug("client-node %p: destroy", impl);

	impl->node.resource = this->resource = NULL;
	spa_hook_remove(&impl->resource_listener);

	if (node->data_source.fd != -1) {
		spa_loop_invoke(node->data_loop,
				do_remove_source,
				SPA_ID_INVALID,
				NULL,
				0,
				true,
				&node->data_source);
	}
	pw_node_destroy(this->node);
}

static void node_initialized(void *data)
{
	struct impl *impl = data;
	struct pw_client_node *this = &impl->this;
	struct pw_node *node = this->node;
	struct pw_type *t = impl->t;
	struct pw_global *global;
	uint32_t node_id;
	uint32_t area_size, size;
	struct mem *m;

	if (this->resource == NULL)
		return;

	impl->fds[0] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	impl->fds[1] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	impl->node.data_source.fd = impl->fds[0];
	impl->node.writefd = impl->fds[1];
	impl->other_fds[0] = impl->fds[1];
	impl->other_fds[1] = impl->fds[0];

	spa_loop_add_source(impl->node.data_loop, &impl->node.data_source);
	pw_log_debug("client-node %p: transport fd %d %d", node, impl->fds[0], impl->fds[1]);

	area_size = sizeof(struct spa_io_buffers) * MAX_AREAS;
	size = area_size + sizeof(struct pw_client_node_position);

	if (pw_memblock_alloc(PW_MEMBLOCK_FLAG_WITH_FD |
			      PW_MEMBLOCK_FLAG_MAP_READWRITE |
			      PW_MEMBLOCK_FLAG_SEAL,
			      size,
			      &impl->io_areas) < 0)
                return;

	impl->position = SPA_MEMBER(impl->io_areas->ptr,
			area_size, struct pw_client_node_position);

	m = ensure_mem(impl, impl->io_areas->fd, t->data.MemFd, impl->io_areas->flags);
	pw_log_debug("client-node %p: io areas %p", node, impl->io_areas->ptr);

	if ((global = pw_node_get_global(node)) != NULL)
		node_id = pw_global_get_id(global);
	else
		node_id = SPA_ID_INVALID;

	pw_client_node_resource_transport(this->resource,
					  node_id,
					  impl->other_fds[0],
					  impl->other_fds[1]);

	pw_client_node_resource_set_position(this->resource,
					     m->id,
					     area_size,
					     sizeof(struct pw_client_node_position));
}

static void node_free(void *data)
{
	struct impl *impl = data;

	pw_log_debug("client-node %p: free", &impl->this);
	node_clear(&impl->node);

	spa_hook_remove(&impl->node_listener);

	pw_array_clear(&impl->mems);
	if (impl->io_areas)
		pw_memblock_free(impl->io_areas);

	pw_map_clear(&impl->io_map);

	if (impl->fds[0] != -1)
		close(impl->fds[0]);
	if (impl->fds[1] != -1)
		close(impl->fds[1]);
	free(impl);
}

static int port_init_mix(void *data, struct pw_port_mix *mix)
{
	struct impl *impl = data;

	mix->id = pw_map_insert_new(&impl->io_map, NULL);

	mix->io = SPA_MEMBER(impl->io_areas->ptr,
			mix->id * sizeof(struct spa_io_buffers), void);
	mix->io->buffer_id = SPA_ID_INVALID;
	mix->io->status = SPA_STATUS_NEED_BUFFER;

	pw_log_debug("client-node %p: init mix io %d %p %p", impl, mix->id, mix->io,
			impl->io_areas->ptr);

	return 0;
}

static int port_release_mix(void *data, struct pw_port_mix *mix)
{
	struct impl *impl = data;

	pw_log_debug("client-node %p: remove mix io %d %p %p", impl, mix->id, mix->io,
			impl->io_areas->ptr);

	pw_map_remove(&impl->io_map, mix->id);
	return 0;
}

static const struct pw_port_implementation port_impl = {
	PW_VERSION_PORT_IMPLEMENTATION,
	.init_mix = port_init_mix,
	.release_mix = port_release_mix,
};

static int mix_port_set_io(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, void *data, size_t size)
{
	struct pw_port *p = SPA_CONTAINER_OF(node, struct pw_port, mix_node);
	struct impl *impl = p->owner_data;
	struct pw_port_mix *mix;

	mix = pw_map_lookup(&p->mix_port_map, port_id);
	if (mix == NULL)
		return -EIO;

	return do_port_set_io(impl,
			      direction, p->port_id, mix,
			      id, data, size);
}

static int mix_port_process(struct spa_node *data)
{
	return SPA_STATUS_HAVE_BUFFER;
}

static void node_port_added(void *data, struct pw_port *port)
{
	struct impl *impl = data;

	pw_log_debug("client-node %p: port %p added", &impl->this, port);
	port->mix_node.port_set_io = mix_port_set_io;
	port->mix_node.process = mix_port_process;

	port->implementation = &port_impl;
	port->implementation_data = impl;

	port->owner_data = impl;
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.free = node_free,
	.initialized = node_initialized,
	.port_added = node_port_added,
};

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = client_node_resource_destroy,
};

/** Create a new client node
 * \param client an owner \ref pw_client
 * \param id an id
 * \param name a name
 * \param properties extra properties
 * \return a newly allocated client node
 *
 * Create a new \ref pw_node.
 *
 * \memberof pw_client_node
 */
struct pw_client_node *pw_client_node_new(struct pw_resource *resource,
					  struct pw_properties *properties,
					  bool do_register)
{
	struct impl *impl;
	struct pw_client_node *this;
	struct pw_client *client = pw_resource_get_client(resource);
	struct pw_core *core = pw_client_get_core(client);
	const struct spa_support *support;
	uint32_t n_support;
	const char *name;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->status = SPA_ID_INVALID;

	impl->core = core;
	impl->t = pw_core_get_type(core);
	impl->fds[0] = impl->fds[1] = -1;
	pw_log_debug("client-node %p: new", impl);

	support = pw_core_get_support(impl->core, &n_support);

	node_init(&impl->node, NULL, support, n_support);
	impl->node.impl = impl;

	pw_map_init(&impl->io_map, 64, 64);
	pw_array_init(&impl->mems, 64);

	if ((name = pw_properties_get(properties, "node.name")) == NULL)
		name = "client-node";

	this->resource = resource;
	this->node = pw_spa_node_new(core,
				     pw_resource_get_client(this->resource),
				     NULL,
				     name,
				     PW_SPA_NODE_FLAG_ASYNC |
				     (do_register ? 0 : PW_SPA_NODE_FLAG_NO_REGISTER),
				     &impl->node.node,
				     NULL,
				     properties, 0);
	if (this->node == NULL)
		goto error_no_node;

	this->node->remote = true;

	pw_resource_add_listener(this->resource,
				 &impl->resource_listener,
				 &resource_events,
				 impl);
	pw_resource_set_implementation(this->resource,
				       &client_node_methods,
				       impl);

	impl->node.resource = this->resource;

	pw_node_add_listener(this->node, &impl->node_listener, &node_events, impl);

	return this;

      error_no_node:
	pw_resource_destroy(this->resource);
	node_clear(&impl->node);
	free(impl);
	return NULL;
}

/** Destroy a client node
 * \param node the client node to destroy
 * \memberof pw_client_node
 */
void pw_client_node_destroy(struct pw_client_node *node)
{
	pw_resource_destroy(node->resource);
}
