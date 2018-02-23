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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <winpr/wlog.h>

#include <freerdp/codec/audio.h>
#include <freerdp/log.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavresample/avresample.h>

#include "rdpsnd_ffmpeg.h"

#define TAG "xxxxxxxxx"

struct rdpsnd_ffmpeg_ctx
{
	enum AVCodecID id;
	AVCodec* codec;
	AVCodecContext* context;
	AVFrame* frame;
	AVFrame* resampledFrame;
	AVPacket* packet;
	AVAudioResampleContext* resampler;
};

static enum AVCodecID rdpsnd_ffmpeg_get_avcodec(const AUDIO_FORMAT* format)
{
	const char* id;

	if (!format)
		return AV_CODEC_ID_NONE;

	id = rdpsnd_get_audio_tag_string(format->wFormatTag);

	switch (format->wFormatTag)
	{
		case WAVE_FORMAT_UNKNOWN:
			return AV_CODEC_ID_NONE;

		case WAVE_FORMAT_PCM:
			switch (format->wBitsPerSample)
			{
				case 16:
					return AV_CODEC_ID_PCM_U16LE;

				case 8:
					return AV_CODEC_ID_PCM_U8;

				default:
					return AV_CODEC_ID_NONE;
			}

		case WAVE_FORMAT_ADPCM:
			return AV_CODEC_ID_ADPCM_MS;

		case WAVE_FORMAT_IEEE_FLOAT:
			return AV_CODEC_ID_PCM_F32LE;

		case WAVE_FORMAT_ALAW:
			return AV_CODEC_ID_PCM_ALAW;

		case WAVE_FORMAT_MULAW:
			return AV_CODEC_ID_PCM_MULAW;

		case WAVE_FORMAT_OKI_ADPCM:
			return AV_CODEC_ID_ADPCM_IMA_OKI;

		case WAVE_FORMAT_G723_ADPCM:
			return AV_CODEC_ID_G723_1;

		case WAVE_FORMAT_GSM610:
			return AV_CODEC_ID_GSM_MS;

		case WAVE_FORMAT_MPEGLAYER3:
			return AV_CODEC_ID_MP3;

		case WAVE_FORMAT_G726_ADPCM:
			return AV_CODEC_ID_ADPCM_G726;

		case WAVE_FORMAT_G722_ADPCM:
			return AV_CODEC_ID_ADPCM_G722;

		case WAVE_FORMAT_G729A:
			return AV_CODEC_ID_G729;

		case WAVE_FORMAT_DOLBY_AC3_SPDIF:
			return AV_CODEC_ID_AC3;

		case WAVE_FORMAT_WMAUDIO2:
			return AV_CODEC_ID_WMAV2;

		case WAVE_FORMAT_WMAUDIO_LOSSLESS:
			return AV_CODEC_ID_WMALOSSLESS;

		case WAVE_FORMAT_AAC_MS:
			return AV_CODEC_ID_AAC;

		default:
			return AV_CODEC_ID_NONE;
	}
}

BOOL rdpnsd_ffmpeg_initialize(void)
{
	avcodec_register_all();
	return TRUE;
}

BOOL rdpnsd_ffmpeg_uninitialize(void)
{
	return TRUE;
}

BOOL rdpsnd_ffmpeg_format_supported(const AUDIO_FORMAT* format)
{
	enum AVCodecID id = rdpsnd_ffmpeg_get_avcodec(format);

	if (id == AV_CODEC_ID_NONE)
		return FALSE;

	return avcodec_find_decoder(id) != NULL;
}

void rdpsnd_ffmpeg_close(RDPSND_FFMPEG* context)
{
	if (context)
	{
		avcodec_free_context(&context->context);
		av_frame_free(&context->frame);
		av_frame_free(&context->resampledFrame);
		av_packet_free(&context->packet);
		avresample_free(&context->resampler);
	}

	free(context);
}

RDPSND_FFMPEG* rdpsnd_ffmpeg_open(const AUDIO_FORMAT* format)
{
	RDPSND_FFMPEG* context = calloc(1, sizeof(RDPSND_FFMPEG));

	if (!context)
		goto fail;

	context->id = rdpsnd_ffmpeg_get_avcodec(format);

	if (context->id == AV_CODEC_ID_NONE)
		goto fail;

	context->codec = avcodec_find_decoder(context->id);

	if (!context->codec)
		goto fail;

	context->context = avcodec_alloc_context3(context->codec);

	if (!context->context)
		goto fail;

	context->context->channels = format->nChannels;
	context->context->sample_rate = format->nSamplesPerSec;
	context->context->block_align = format->nBlockAlign;
	context->context->bit_rate = format->nAvgBytesPerSec * 8;

	if (avcodec_open2(context->context, context->codec, NULL) < 0)
		goto fail;

	context->packet = av_packet_alloc();

	if (!context->packet)
		goto fail;

	context->frame = av_frame_alloc();

	if (!context->frame)
		goto fail;

	context->resampledFrame = av_frame_alloc();

	if (!context->resampledFrame)
		goto fail;

	context->resampler = avresample_alloc_context();

	if (!context->resampler)
		goto fail;

	int layout;

	switch (format->nChannels)
	{
		case 1:
			layout = AV_CH_LAYOUT_MONO;
			break;

		case 2:
			layout = AV_CH_LAYOUT_STEREO;
			break;

		default:
			layout = AV_CH_LAYOUT_5POINT1;
			break;
	}

	context->resampledFrame->channel_layout = layout;
	context->resampledFrame->channels = format->nChannels;
	context->resampledFrame->sample_rate = format->nSamplesPerSec;
	context->resampledFrame->format = AV_SAMPLE_FMT_S16;
	return context;
fail:
	rdpsnd_ffmpeg_close(context);
	return NULL;
}

static BOOL ffmpeg_decode(AVCodecContext* dec_ctx, AVPacket* pkt,
                          AVFrame* frame,
                          AVAudioResampleContext* resampleContext,
                          AVFrame* resampled, wStream* out)
{
	int ret;
	/* send the packet with the compressed data to the decoder */
	ret = avcodec_send_packet(dec_ctx, pkt);

	if (ret < 0)
	{
		const char* err = av_err2str(ret);
		WLog_ERR(TAG, "Error submitting the packet to the decoder %s [%d]",
		         err, ret);
		return FALSE;
	}

	/* read all the output frames (in general there may be any number of them */
	while (ret >= 0)
	{
		ret = avcodec_receive_frame(dec_ctx, frame);

		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return TRUE;
		else if (ret < 0)
		{
			const char* err = av_err2str(ret);
			WLog_ERR(TAG, "Error during decoding %s [%d]", err, ret);
			return FALSE;
		}

		if (!avresample_is_open(resampleContext))
		{
			if ((ret = avresample_config(resampleContext, resampled, frame)) < 0)
			{
				const char* err = av_err2str(ret);
				WLog_ERR(TAG, "Error during resampling %s [%d]", err, ret);
				return FALSE;
			}

			if ((ret = (avresample_open(resampleContext))) < 0)
			{
				const char* err = av_err2str(ret);
				WLog_ERR(TAG, "Error during resampling %s [%d]", err, ret);
				return FALSE;
			}
		}

		if ((ret = avresample_convert_frame(resampleContext, resampled, frame)) < 0)
		{
			const char* err = av_err2str(ret);
			WLog_ERR(TAG, "Error during resampling %s [%d]", err, ret);
			return FALSE;
		}

		{
			const size_t data_size = resampled->channels * resampled->nb_samples * 2;
			Stream_EnsureRemainingCapacity(out, data_size);
			Stream_Write(out, resampled->data[0], data_size);
		}
	}

	return TRUE;
}

size_t rdpsnd_ffmpeg_decode(RDPSND_FFMPEG* context, const BYTE* data, size_t size,
                            wStream* out, UINT16* sample_length)
{
	size_t s;

	if (!context || !data || !out)
		return 0;

	av_init_packet(context->packet);
	context->packet->data = data;
	context->packet->size = size;
	s = ffmpeg_decode(context->context, context->packet, context->frame,
	                  context->resampler, context->resampledFrame, out);
	*sample_length = context->resampledFrame->nb_samples * 1000 / context->resampledFrame->sample_rate /
	                 context->resampledFrame->channels;
	return s;
}
