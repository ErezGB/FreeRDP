/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Digital Sound Processing
 *
 * Copyright 2010-2011 Vic Lee
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/crt.h>

#include <freerdp/types.h>
#include <freerdp/codec/dsp.h>

#if defined(WITH_GSM)
#include <gsm/gsm.h>
#endif

#if defined(WITH_FFMPEG)
#include "dsp_ffmpeg.h"
#endif

union _ADPCM
{
	struct
	{
		INT16 last_sample[2];
		INT16 last_step[2];
	} ima;
	struct
	{
		BYTE predictor[2];
		INT32 delta[2];
		INT32 sample1[2];
		INT32 sample2[2];
	} ms;
};
typedef union _ADPCM ADPCM;

struct _FREERDP_DSP_CONTEXT
{
	BOOL encoder;

	ADPCM adpcm;
	AUDIO_FORMAT format;

	wStream* buffer;
	wStream* resample;

#if defined(WITH_GSM)
	gsm gsm;
#endif
};

/**
 * Microsoft Multimedia Standards Update
 * http://download.microsoft.com/download/9/8/6/9863C72A-A3AA-4DDB-B1BA-CA8D17EFD2D4/RIFFNEW.pdf
 */

static BOOL freerdp_dsp_resample(FREERDP_DSP_CONTEXT* context,
                                 const BYTE* src, size_t bytes_per_sample,
                                 UINT32 schan, UINT32 srate, size_t sframes,
                                 UINT32 rchan, UINT32 rrate)
{
	BYTE* p;
	int rframes;
	int rsize;
	int i, j;
	int n1, n2;
	int sbytes, rbytes;
	sbytes = bytes_per_sample * schan;
	rbytes = bytes_per_sample * rchan;
	rframes = sframes * rrate / srate;
	rsize = rbytes * rframes;

	if (!Stream_EnsureCapacity(context->resample, rsize + 1024))
		return FALSE;

	p = Stream_Buffer(context->resample);

	for (i = 0; i < rframes; i++)
	{
		n1 = i * srate / rrate;

		if (n1 >= sframes)
			n1 = sframes - 1;

		n2 = (n1 * rrate == i * srate || n1 == sframes - 1 ? n1 : n1 + 1);

		for (j = 0; j < rbytes; j++)
		{
			/* Nearest Interpolation, probably the easiest, but works */
			*p++ = (i * srate - n1 * rrate > n2 * rrate - i * srate ?
			        src[n2 * sbytes + (j % sbytes)] :
			        src[n1 * sbytes + (j % sbytes)]);
		}
	}

	Stream_SetPointer(context->resample, p);
	Stream_SealLength(context->resample);
	return TRUE;
}

/**
 * Microsoft IMA ADPCM specification:
 *
 * http://wiki.multimedia.cx/index.php?title=Microsoft_IMA_ADPCM
 * http://wiki.multimedia.cx/index.php?title=IMA_ADPCM
 */

static const INT16 ima_step_index_table[] =
{
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8
};

static const INT16 ima_step_size_table[] =
{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
	19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
	337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
	5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static UINT16 dsp_decode_ima_adpcm_sample(ADPCM* adpcm,
        unsigned int channel, BYTE sample)
{
	INT32 ss;
	INT32 d;
	ss = ima_step_size_table[adpcm->ima.last_step[channel]];
	d = (ss >> 3);

	if (sample & 1)
		d += (ss >> 2);

	if (sample & 2)
		d += (ss >> 1);

	if (sample & 4)
		d += ss;

	if (sample & 8)
		d = -d;

	d += adpcm->ima.last_sample[channel];

	if (d < -32768)
		d = -32768;
	else if (d > 32767)
		d = 32767;

	adpcm->ima.last_sample[channel] = (INT16) d;
	adpcm->ima.last_step[channel] += ima_step_index_table[sample];

	if (adpcm->ima.last_step[channel] < 0)
		adpcm->ima.last_step[channel] = 0;
	else if (adpcm->ima.last_step[channel] > 88)
		adpcm->ima.last_step[channel] = 88;

	return (UINT16) d;
}

static BOOL freerdp_dsp_decode_ima_adpcm(FREERDP_DSP_CONTEXT* context,
        const BYTE* src, size_t size, wStream* out)
{
	BYTE* dst;
	BYTE sample;
	UINT16 decoded;
	UINT32 out_size = size * 4;
	UINT32 channel;
	const UINT32 block_size = context->format.nBlockAlign;
	const UINT32 channels = context->format.nChannels;
	int i;

	if (!Stream_EnsureCapacity(out, out_size))
		return FALSE;

	dst = Stream_Buffer(out);

	while (size > 0)
	{
		if (size % block_size == 0)
		{
			context->adpcm.ima.last_sample[0] = (INT16)(((UINT16)(*src)) | (((UINT16)(*(src + 1))) << 8));
			context->adpcm.ima.last_step[0] = (INT16)(*(src + 2));
			src += 4;
			size -= 4;
			out_size -= 16;

			if (channels > 1)
			{
				context->adpcm.ima.last_sample[1] = (INT16)(((UINT16)(*src)) | (((UINT16)(*(src + 1))) << 8));
				context->adpcm.ima.last_step[1] = (INT16)(*(src + 2));
				src += 4;
				size -= 4;
				out_size -= 16;
			}
		}

		if (channels > 1)
		{
			for (i = 0; i < 8; i++)
			{
				channel = (i < 4 ? 0 : 1);
				sample = ((*src) & 0x0f);
				decoded = dsp_decode_ima_adpcm_sample(&context->adpcm, channel, sample);
				dst[((i & 3) << 3) + (channel << 1)] = (decoded & 0xFF);
				dst[((i & 3) << 3) + (channel << 1) + 1] = (decoded >> 8);
				sample = ((*src) >> 4);
				decoded = dsp_decode_ima_adpcm_sample(&context->adpcm, channel, sample);
				dst[((i & 3) << 3) + (channel << 1) + 4] = (decoded & 0xFF);
				dst[((i & 3) << 3) + (channel << 1) + 5] = (decoded >> 8);
				src++;
			}

			dst += 32;
			size -= 8;
		}
		else
		{
			sample = ((*src) & 0x0f);
			decoded = dsp_decode_ima_adpcm_sample(&context->adpcm, 0, sample);
			*dst++ = (decoded & 0xFF);
			*dst++ = (decoded >> 8);
			sample = ((*src) >> 4);
			decoded = dsp_decode_ima_adpcm_sample(&context->adpcm, 0, sample);
			*dst++ = (decoded & 0xFF);
			*dst++ = (decoded >> 8);
			src++;
			size--;
		}
	}

	Stream_SetPointer(out, dst);
	return TRUE;
}

#if defined(WITH_GSM)
static BOOL freerdp_dsp_decode_gsm610(FREERDP_DSP_CONTEXT* context,
                                      const BYTE* src, size_t size, wStream* out)
{
	size_t offset = 0;

	while (offset < size)
	{
		gsm_signal gsmBlockBuffer[160] = { 0 };
		gsm_decode(context->gsm, (gsm_byte*) &src[offset], gsmBlockBuffer);
		offset += sizeof(gsm_frame);

		if (!Stream_EnsureRemainingCapacity(out, sizeof(gsmBlockBuffer)))
			return FALSE;

		Stream_Write(out, (void*) gsmBlockBuffer, sizeof(gsmBlockBuffer));
	}

	return TRUE;
}

static BOOL freerdp_dsp_encode_gsm610(FREERDP_DSP_CONTEXT* context,
                                      const BYTE* src, size_t size, wStream* out)
{
	{
		size_t offset = 0;

		while (offset < size)
		{
			gsm_signal* signal = (gsm_signal*)&src[offset];

			if (!Stream_EnsureRemainingCapacity(out, sizeof(gsm_frame)))
				return FALSE;

			gsm_encode(context->gsm, signal, Stream_Pointer(out));
			Stream_Seek(out, sizeof(gsm_frame));
			offset += 160;
		}

		return TRUE;
	}
}
#endif

/**
 * 0     1     2     3
 * 2 0   6 4   10 8  14 12   <left>
 *
 * 4     5     6     7
 * 3 1   7 5   11 9  15 13   <right>
 */
static const struct
{
	BYTE byte_num;
	BYTE byte_shift;
} ima_stereo_encode_map[] =
{
	{ 0, 0 },
	{ 4, 0 },
	{ 0, 4 },
	{ 4, 4 },
	{ 1, 0 },
	{ 5, 0 },
	{ 1, 4 },
	{ 5, 4 },
	{ 2, 0 },
	{ 6, 0 },
	{ 2, 4 },
	{ 6, 4 },
	{ 3, 0 },
	{ 7, 0 },
	{ 3, 4 },
	{ 7, 4 }
};

static BYTE dsp_encode_ima_adpcm_sample(ADPCM* adpcm, int channel, INT16 sample)
{
	INT32 e;
	INT32 d;
	INT32 ss;
	BYTE enc;
	INT32 diff;
	ss = ima_step_size_table[adpcm->ima.last_step[channel]];
	d = e = sample - adpcm->ima.last_sample[channel];
	diff = ss >> 3;
	enc = 0;

	if (e < 0)
	{
		enc = 8;
		e = -e;
	}

	if (e >= ss)
	{
		enc |= 4;
		e -= ss;
	}

	ss >>= 1;

	if (e >= ss)
	{
		enc |= 2;
		e -= ss;
	}

	ss >>= 1;

	if (e >= ss)
	{
		enc |= 1;
		e -= ss;
	}

	if (d < 0)
		diff = d + e - diff;
	else
		diff = d - e + diff;

	diff += adpcm->ima.last_sample[channel];

	if (diff < -32768)
		diff = -32768;
	else if (diff > 32767)
		diff = 32767;

	adpcm->ima.last_sample[channel] = (INT16) diff;
	adpcm->ima.last_step[channel] += ima_step_index_table[enc];

	if (adpcm->ima.last_step[channel] < 0)
		adpcm->ima.last_step[channel] = 0;
	else if (adpcm->ima.last_step[channel] > 88)
		adpcm->ima.last_step[channel] = 88;

	return enc;
}

static BOOL freerdp_dsp_encode_ima_adpcm(FREERDP_DSP_CONTEXT* context,
        const BYTE* src, size_t size, wStream* out)
{
	int i;
	BYTE* dst;
	INT16 sample;
	BYTE encoded;
	UINT32 out_size;
	out_size = size / 2;

	if (!Stream_EnsureRemainingCapacity(out, out_size))
		return FALSE;

	dst = Stream_Buffer(out);

	while (size > 0)
	{
		if ((dst - Stream_Buffer(out)) % context->format.nBlockAlign == 0)
		{
			*dst++ = context->adpcm.ima.last_sample[0] & 0xFF;
			*dst++ = (context->adpcm.ima.last_sample[0] >> 8) & 0xFF;
			*dst++ = (BYTE) context->adpcm.ima.last_step[0];
			*dst++ = 0;

			if (context->format.nChannels > 1)
			{
				*dst++ = context->adpcm.ima.last_sample[1] & 0xFF;
				*dst++ = (context->adpcm.ima.last_sample[1] >> 8) & 0xFF;
				*dst++ = (BYTE) context->adpcm.ima.last_step[1];
				*dst++ = 0;
			}
		}

		if (context->format.nChannels > 1)
		{
			ZeroMemory(dst, 8);

			for (i = 0; i < 16; i++)
			{
				sample = (INT16)(((UINT16)(*src)) | (((UINT16)(*(src + 1))) << 8));
				src += 2;
				encoded = dsp_encode_ima_adpcm_sample(&context->adpcm, i % 2, sample);
				dst[ima_stereo_encode_map[i].byte_num] |= encoded << ima_stereo_encode_map[i].byte_shift;
			}

			dst += 8;
			size -= 32;
		}
		else
		{
			sample = (INT16)(((UINT16)(*src)) | (((UINT16)(*(src + 1))) << 8));
			src += 2;
			encoded = dsp_encode_ima_adpcm_sample(&context->adpcm, 0, sample);
			sample = (INT16)(((UINT16)(*src)) | (((UINT16)(*(src + 1))) << 8));
			src += 2;
			encoded |= dsp_encode_ima_adpcm_sample(&context->adpcm, 0, sample) << 4;
			*dst++ = encoded;
			size -= 4;
		}
	}

	Stream_SetPointer(out, dst);
	return TRUE;
}

/**
 * Microsoft ADPCM Specification:
 *
 * http://wiki.multimedia.cx/index.php?title=Microsoft_ADPCM
 */

static const INT32 ms_adpcm_adaptation_table[] =
{
	230, 230, 230, 230, 307, 409, 512, 614,
	768, 614, 512, 409, 307, 230, 230, 230
};

static const INT32 ms_adpcm_coeffs1[7] =
{
	256, 512, 0, 192, 240, 460, 392
};

static const INT32 ms_adpcm_coeffs2[7] =
{
	0, -256, 0, 64, 0, -208, -232
};

static INLINE INT16 freerdp_dsp_decode_ms_adpcm_sample(ADPCM* adpcm, BYTE sample, int channel)
{
	INT8 nibble;
	INT32 presample;
	nibble = (sample & 0x08 ? (INT8) sample - 16 : sample);
	presample = ((adpcm->ms.sample1[channel] * ms_adpcm_coeffs1[adpcm->ms.predictor[channel]]) +
	             (adpcm->ms.sample2[channel] * ms_adpcm_coeffs2[adpcm->ms.predictor[channel]])) / 256;
	presample += nibble * adpcm->ms.delta[channel];

	if (presample > 32767)
		presample = 32767;
	else if (presample < -32768)
		presample = -32768;

	adpcm->ms.sample2[channel] = adpcm->ms.sample1[channel];
	adpcm->ms.sample1[channel] = presample;
	adpcm->ms.delta[channel] = adpcm->ms.delta[channel] * ms_adpcm_adaptation_table[sample] / 256;

	if (adpcm->ms.delta[channel] < 16)
		adpcm->ms.delta[channel] = 16;

	return (INT16) presample;
}

static BOOL freerdp_dsp_decode_ms_adpcm(FREERDP_DSP_CONTEXT* context,
                                        const BYTE* src, size_t size, wStream* out)
{
	BYTE* dst;
	BYTE sample;
	const UINT32 out_size = size * 4;
	const UINT32 channels = context->format.nChannels;
	const UINT32 block_size = context->format.nBlockAlign;

	if (!Stream_EnsureCapacity(out, out_size))
		return FALSE;

	dst = Stream_Buffer(out);

	while (size > 0)
	{
		if (size % block_size == 0)
		{
			if (channels > 1)
			{
				context->adpcm.ms.predictor[0] = *src++;
				context->adpcm.ms.predictor[1] = *src++;
				context->adpcm.ms.delta[0] = *((INT16*) src);
				src += 2;
				context->adpcm.ms.delta[1] = *((INT16*) src);
				src += 2;
				context->adpcm.ms.sample1[0] = *((INT16*) src);
				src += 2;
				context->adpcm.ms.sample1[1] = *((INT16*) src);
				src += 2;
				context->adpcm.ms.sample2[0] = *((INT16*) src);
				src += 2;
				context->adpcm.ms.sample2[1] = *((INT16*) src);
				src += 2;
				size -= 14;
				*((INT16*) dst) = context->adpcm.ms.sample2[0];
				dst += 2;
				*((INT16*) dst) = context->adpcm.ms.sample2[1];
				dst += 2;
				*((INT16*) dst) = context->adpcm.ms.sample1[0];
				dst += 2;
				*((INT16*) dst) = context->adpcm.ms.sample1[1];
				dst += 2;
			}
			else
			{
				context->adpcm.ms.predictor[0] = *src++;
				context->adpcm.ms.delta[0] = *((INT16*) src);
				src += 2;
				context->adpcm.ms.sample1[0] = *((INT16*) src);
				src += 2;
				context->adpcm.ms.sample2[0] = *((INT16*) src);
				src += 2;
				size -= 7;
				*((INT16*) dst) = context->adpcm.ms.sample2[0];
				dst += 2;
				*((INT16*) dst) = context->adpcm.ms.sample1[0];
				dst += 2;
			}
		}

		if (channels > 1)
		{
			sample = *src++;
			size--;
			*((INT16*) dst) = freerdp_dsp_decode_ms_adpcm_sample(&context->adpcm, sample >> 4, 0);
			dst += 2;
			*((INT16*) dst) = freerdp_dsp_decode_ms_adpcm_sample(&context->adpcm, sample & 0x0F, 1);
			dst += 2;
			sample = *src++;
			size--;
			*((INT16*) dst) = freerdp_dsp_decode_ms_adpcm_sample(&context->adpcm, sample >> 4, 0);
			dst += 2;
			*((INT16*) dst) = freerdp_dsp_decode_ms_adpcm_sample(&context->adpcm, sample & 0x0F, 1);
			dst += 2;
		}
		else
		{
			sample = *src++;
			size--;
			*((INT16*) dst) = freerdp_dsp_decode_ms_adpcm_sample(&context->adpcm, sample >> 4, 0);
			dst += 2;
			*((INT16*) dst) = freerdp_dsp_decode_ms_adpcm_sample(&context->adpcm, sample & 0x0F, 0);
			dst += 2;
		}
	}

	Stream_SetPointer(out, dst);
	return TRUE;
}

static BYTE freerdp_dsp_encode_ms_adpcm_sample(ADPCM* adpcm, INT32 sample, int channel)
{
	INT32 presample;
	INT32 errordelta;
	presample = ((adpcm->ms.sample1[channel] * ms_adpcm_coeffs1[adpcm->ms.predictor[channel]]) +
	             (adpcm->ms.sample2[channel] * ms_adpcm_coeffs2[adpcm->ms.predictor[channel]])) / 256;
	errordelta = (sample - presample) / adpcm->ms.delta[channel];

	if ((sample - presample) % adpcm->ms.delta[channel] > adpcm->ms.delta[channel] / 2)
		errordelta++;

	if (errordelta > 7)
		errordelta = 7;
	else if (errordelta < -8)
		errordelta = -8;

	presample += adpcm->ms.delta[channel] * errordelta;

	if (presample > 32767)
		presample = 32767;
	else if (presample < -32768)
		presample = -32768;

	adpcm->ms.sample2[channel] = adpcm->ms.sample1[channel];
	adpcm->ms.sample1[channel] = presample;
	adpcm->ms.delta[channel] = adpcm->ms.delta[channel] * ms_adpcm_adaptation_table[(((
	                               BYTE) errordelta) & 0x0F)] / 256;

	if (adpcm->ms.delta[channel] < 16)
		adpcm->ms.delta[channel] = 16;

	return ((BYTE) errordelta) & 0x0F;
}

static BOOL freerdp_dsp_encode_ms_adpcm(FREERDP_DSP_CONTEXT* context, const BYTE* src, size_t size,
                                        wStream* out)
{
	BYTE* dst;
	INT32 sample;
	UINT32 out_size;
	out_size = size / 2;

	if (!Stream_EnsureRemainingCapacity(out, out_size))
		return FALSE;

	dst = Stream_Buffer(out);

	if (context->adpcm.ms.delta[0] < 16)
		context->adpcm.ms.delta[0] = 16;

	if (context->adpcm.ms.delta[1] < 16)
		context->adpcm.ms.delta[1] = 16;

	while (size > 0)
	{
		if ((dst - Stream_Buffer(out)) % context->format.nBlockAlign == 0)
		{
			if (context->format.nChannels > 1)
			{
				*dst++ = context->adpcm.ms.predictor[0];
				*dst++ = context->adpcm.ms.predictor[1];
				*dst++ = (BYTE)(context->adpcm.ms.delta[0] & 0xFF);
				*dst++ = (BYTE)((context->adpcm.ms.delta[0] >> 8) & 0xFF);
				*dst++ = (BYTE)(context->adpcm.ms.delta[1] & 0xFF);
				*dst++ = (BYTE)((context->adpcm.ms.delta[1] >> 8) & 0xFF);
				context->adpcm.ms.sample1[0] = *((INT16*)(src + 4));
				context->adpcm.ms.sample1[1] = *((INT16*)(src + 6));
				context->adpcm.ms.sample2[0] = *((INT16*)(src + 0));
				context->adpcm.ms.sample2[1] = *((INT16*)(src + 2));
				*((INT16*)(dst + 0)) = (INT16) context->adpcm.ms.sample1[0];
				*((INT16*)(dst + 2)) = (INT16) context->adpcm.ms.sample1[1];
				*((INT16*)(dst + 4)) = (INT16) context->adpcm.ms.sample2[0];
				*((INT16*)(dst + 6)) = (INT16) context->adpcm.ms.sample2[1];
				dst += 8;
				src += 8;
				size -= 8;
			}
			else
			{
				*dst++ = context->adpcm.ms.predictor[0];
				*dst++ = (BYTE)(context->adpcm.ms.delta[0] & 0xFF);
				*dst++ = (BYTE)((context->adpcm.ms.delta[0] >> 8) & 0xFF);
				context->adpcm.ms.sample1[0] = *((INT16*)(src + 2));
				context->adpcm.ms.sample2[0] = *((INT16*)(src + 0));
				*((INT16*)(dst + 0)) = (INT16) context->adpcm.ms.sample1[0];
				*((INT16*)(dst + 2)) = (INT16) context->adpcm.ms.sample2[0];
				dst += 4;
				src += 4;
				size -= 4;
			}
		}

		sample = *((INT16*) src);
		src += 2;
		*dst = freerdp_dsp_encode_ms_adpcm_sample(&context->adpcm, sample, 0) << 4;
		sample = *((INT16*) src);
		src += 2;
		*dst += freerdp_dsp_encode_ms_adpcm_sample(&context->adpcm, sample,
		        context->format.nChannels > 1 ? 1 : 0);
		dst++;
		size -= 4;
	}

	Stream_SetPointer(out, dst);
	return TRUE;
}

FREERDP_DSP_CONTEXT* freerdp_dsp_context_new(BOOL encoder)
{
#if defined(WITH_FFMPEG)
	return freerdp_dsp_ffmpeg_context_new(encoder);
#else
	FREERDP_DSP_CONTEXT* context = calloc(1, sizeof(FREERDP_DSP_CONTEXT));

	if (!context)
		return NULL;

	context->encoder = encoder;
#if defined(WITH_GSM)
	context->gsm = gsm_create();

	if (!context->gsm)
	{
		free(context);
		return NULL;
	}

#endif
	return context;
#endif
}

void freerdp_dsp_context_free(FREERDP_DSP_CONTEXT* context)
{
#if defined(WITH_FFMPEG)
	freerdp_dsp_ffmpeg_context_free(context);
#else

	if (context)
	{
#if defined(WITH_GSM)
		gsm_destroy(context->gsm);
#endif
		free(context);
	}

#endif
}

BOOL freerdp_dsp_encode(FREERDP_DSP_CONTEXT* context, const AUDIO_FORMAT* srcFormat,
                        const BYTE* data, size_t length, wStream* out)
{
#if defined(WITH_FFMPEG)
	return freerdp_dsp_ffmpeg_encode(context, srcFormat, data, length, out);
#else

	if (!context || !context->encoder || !srcFormat || !data || !out)
		return FALSE;

	// TODO: Resample

	switch (context->format.wFormatTag)
	{
		case WAVE_FORMAT_PCM:
			if (!Stream_EnsureRemainingCapacity(out, length))
				return FALSE;

			Stream_Write(out, data, length);
			return TRUE;

		case WAVE_FORMAT_ADPCM:
			return freerdp_dsp_encode_ms_adpcm(context, data, length, out);

		case WAVE_FORMAT_DVI_ADPCM:
			return freerdp_dsp_encode_ima_adpcm(context, data, length, out);
#if defined(WITH_GSM)

		case WAVE_FORMAT_GSM610:
			return freerdp_dsp_encode_gsm610(context, data, length, out);
#endif

		default:
			return FALSE;
	}

	return FALSE;
#endif
}

BOOL freerdp_dsp_decode(FREERDP_DSP_CONTEXT* context, const AUDIO_FORMAT* srcFormat,
                        const BYTE* data, size_t length, wStream* out)
{
#if defined(WITH_FFMPEG)
	return freerdp_dsp_ffmpeg_decode(context, srcFormat, data, length, out);
#else

	if (!context || context->encoder || !srcFormat || !data || !out)
		return FALSE;

	switch (context->format.wFormatTag)
	{
		case WAVE_FORMAT_PCM:
			if (!Stream_EnsureRemainingCapacity(out, length))
				return FALSE;

			Stream_Write(out, data, length);
			return TRUE;

		case WAVE_FORMAT_ADPCM:
			return freerdp_dsp_decode_ms_adpcm(context, data, length, out);

		case WAVE_FORMAT_DVI_ADPCM:
			return freerdp_dsp_decode_ima_adpcm(context, data, length, out);
#if defined(WITH_GSM)

		case WAVE_FORMAT_GSM610:
			return freerdp_dsp_decode_gsm610(context, data, length, out);
#endif

		default:
			return FALSE;
	}

	return FALSE;
#endif
}

BOOL freerdp_dsp_supports_format(const AUDIO_FORMAT* format, BOOL encode)
{
#if defined(WITH_FFMPEG)
	return freerdp_dsp_ffmpeg_supports_format(format, encode);
#else

	switch (format->wFormatTag)
	{
		case WAVE_FORMAT_PCM:
		case WAVE_FORMAT_ADPCM:
		case WAVE_FORMAT_DVI_ADPCM:
			return TRUE;
#if defined(WITH_GSM)

		case WAVE_FORMAT_GSM610:
			return TRUE;
#endif

		default:
			return FALSE;
	}

	return FALSE;
#endif
}


BOOL freerdp_dsp_context_reset(FREERDP_DSP_CONTEXT* context, const AUDIO_FORMAT* targetFormat)
{
#if defined(WITH_FFMPEG)
	return freerdp_dsp_ffmpeg_context_reset(context, targetFormat);
#else

	if (!context || !targetFormat)
		return FALSE;

	context->format = *targetFormat;
	return TRUE;
#endif
}
