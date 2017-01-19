/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_NODE_EVENT_H__
#define __SPA_NODE_EVENT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaNodeEvent SpaNodeEvent;

#include <spa/defs.h>
#include <spa/node.h>

#define SPA_NODE_EVENT_URI             "http://spaplug.in/ns/node-event"
#define SPA_NODE_EVENT_PREFIX          SPA_NODE_EVENT_URI "#"

#define SPA_NODE_EVENT__AsyncComplete         SPA_NODE_EVENT_PREFIX "AsyncComplete"
#define SPA_NODE_EVENT__HaveOutput            SPA_NODE_EVENT_PREFIX "HaveOutput"
#define SPA_NODE_EVENT__NeedInput             SPA_NODE_EVENT_PREFIX "NeedInput"
#define SPA_NODE_EVENT__ReuseBuffer           SPA_NODE_EVENT_PREFIX "ReuseBuffer"
#define SPA_NODE_EVENT__Error                 SPA_NODE_EVENT_PREFIX "Error"
#define SPA_NODE_EVENT__Buffering             SPA_NODE_EVENT_PREFIX "Buffering"
#define SPA_NODE_EVENT__RequestRefresh        SPA_NODE_EVENT_PREFIX "RequestRefresh"
#define SPA_NODE_EVENT__RequestClockUpdate    SPA_NODE_EVENT_PREFIX "RequestClockUpdate"

/**
 * SpaEventType:
 * @SPA_NODE_EVENT_TYPE_INVALID: invalid event, should be ignored
 * @SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE: an async operation completed
 * @SPA_NODE_EVENT_TYPE_HAVE_OUTPUT: emited when an async node has output that can be pulled
 * @SPA_NODE_EVENT_TYPE_NEED_INPUT: emited when more data can be pushed to an async node
 * @SPA_NODE_EVENT_TYPE_REUSE_BUFFER: emited when a buffer can be reused
 * @SPA_NODE_EVENT_TYPE_ERROR: emited when error occured
 * @SPA_NODE_EVENT_TYPE_BUFFERING: emited when buffering is in progress
 * @SPA_NODE_EVENT_TYPE_REQUEST_REFRESH: emited when a keyframe refresh is needed
 * @SPA_NODE_EVENT_TYPE_REQUEST_CLOCK_UPDATE: the element asks for a clock update
 */
typedef enum {
  SPA_NODE_EVENT_TYPE_INVALID                  = 0,
  SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE,
  SPA_NODE_EVENT_TYPE_HAVE_OUTPUT,
  SPA_NODE_EVENT_TYPE_NEED_INPUT,
  SPA_NODE_EVENT_TYPE_REUSE_BUFFER,
  SPA_NODE_EVENT_TYPE_ERROR,
  SPA_NODE_EVENT_TYPE_BUFFERING,
  SPA_NODE_EVENT_TYPE_REQUEST_REFRESH,
  SPA_NODE_EVENT_TYPE_REQUEST_CLOCK_UPDATE,
} SpaNodeEventType;

struct _SpaNodeEvent {
  SpaNodeEventType  type;
  size_t            size;
};

typedef struct {
  SpaNodeEvent event;
  uint32_t     seq;
  SpaResult    res;
} SpaNodeEventAsyncComplete;

typedef struct {
  SpaNodeEvent event;
  uint32_t port_id;
  uint32_t buffer_id;
} SpaNodeEventReuseBuffer;

typedef struct {
  SpaNodeEvent event;
#define SPA_NODE_EVENT_REQUEST_CLOCK_UPDATE_TIME        (1 << 0)
#define SPA_NODE_EVENT_REQUEST_CLOCK_UPDATE_SCALE       (1 << 1)
#define SPA_NODE_EVENT_REQUEST_CLOCK_UPDATE_STATE       (1 << 2)
  uint32_t      update_mask;
  int64_t       timestamp;
  int64_t       offset;
} SpaNodeEventRequestClockUpdate;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_NODE_EVENT_H__ */
