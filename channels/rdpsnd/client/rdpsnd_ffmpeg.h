/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Audio Output Virtual Channel - FFMPEG decoder
 *
 * Copyright 2018 Armin Novak <armin.novak@thincast.com>
 * Copyright 2018 Thincast Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FREERDP_CHANNEL_RDPSND_CLIENT_FFMPEG_H
#define FREERDP_CHANNEL_RDPSND_CLIENT_FFMPEG_H

#include <winpr/stream.h>

#include <freerdp/api.h>
#include <freerdp/codec/audio.h>

typedef struct rdpsnd_ffmpeg_ctx RDPSND_FFMPEG;

BOOL rdpnsd_ffmpeg_initialize(void);
BOOL rdpnsd_ffmpeg_uninitialize(void);

BOOL rdpsnd_ffmpeg_format_supported(const AUDIO_FORMAT* format);

RDPSND_FFMPEG* rdpsnd_ffmpeg_open(const AUDIO_FORMAT* format);
void rdpsnd_ffmpeg_close(RDPSND_FFMPEG* context);

size_t rdpsnd_ffmpeg_decode(RDPSND_FFMPEG* context,
                            const BYTE* data, size_t size,
                            wStream* out, UINT16* sample_length);

#endif /* FREERDP_CHANNEL_RDPSND_CLIENT_FFMPEG_H */
