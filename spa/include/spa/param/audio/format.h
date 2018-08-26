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

#ifndef __SPA_PARAM_AUDIO_FORMAT_H__
#define __SPA_PARAM_AUDIO_FORMAT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/format.h>
#include <spa/param/audio/raw.h>

struct spa_audio_info {
	uint32_t media_type;
	uint32_t media_subtype;
	union {
		struct spa_audio_info_raw raw;
	} info;
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_AUDIO_FORMAT_H */
