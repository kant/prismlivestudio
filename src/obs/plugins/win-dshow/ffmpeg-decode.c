/******************************************************************************
    Copyright (C) 2014 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "ffmpeg-decode.h"
#include "obs-ffmpeg-compat.h"
#include <obs-avc.h>

#if LIBAVCODEC_VERSION_INT > AV_VERSION_INT(58, 4, 100)
#define USE_NEW_HARDWARE_CODEC_METHOD
#endif

#ifdef USE_NEW_HARDWARE_CODEC_METHOD
enum AVHWDeviceType hw_priority[] = {
	AV_HWDEVICE_TYPE_NONE,
};

static bool has_hw_type(AVCodec *c, enum AVHWDeviceType type)
{
	for (int i = 0;; i++) {
		const AVCodecHWConfig *config = avcodec_get_hw_config(c, i);
		if (!config) {
			break;
		}

		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
		    config->device_type == type)
			return true;
	}

	return false;
}

static void init_hw_decoder(struct ffmpeg_decode *d)
{
	enum AVHWDeviceType *priority = hw_priority;
	AVBufferRef *hw_ctx = NULL;

	while (*priority != AV_HWDEVICE_TYPE_NONE) {
		if (has_hw_type(d->codec, *priority)) {
			int ret = av_hwdevice_ctx_create(&hw_ctx, *priority,
							 NULL, NULL, 0);
			if (ret == 0)
				break;
		}

		priority++;
	}

	if (hw_ctx) {
		d->decoder->hw_device_ctx = av_buffer_ref(hw_ctx);
		d->hw = true;
	}
}
#endif

int ffmpeg_decode_init(struct ffmpeg_decode *decode, enum AVCodecID id,
		       bool use_hw, uint8_t *extra_data, int extra_data_size)
{
	int ret;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
	avcodec_register_all();
#endif
	memset(decode, 0, sizeof(*decode));

	decode->codec = avcodec_find_decoder(id);
	if (!decode->codec)
		return -1;

	decode->decoder = avcodec_alloc_context3(decode->codec);

	decode->decoder->thread_count = 0;

#ifdef USE_NEW_HARDWARE_CODEC_METHOD
	if (use_hw)
		init_hw_decoder(decode);
#else
	(void)use_hw;
#endif
	//PRISM/LiuHaibin/20210304/#None/add extra data and extra data size
	if (extra_data && extra_data_size > 0) {
		size_t new_size = extra_data_size + INPUT_BUFFER_PADDING_SIZE;

		if (decode->extra_data_size < new_size) {
			decode->extra_data =
				brealloc(decode->extra_data, new_size);
			decode->extra_data_size = new_size;
		}
		memset(decode->extra_data + extra_data_size, 0,
		       INPUT_BUFFER_PADDING_SIZE);
		memcpy(decode->extra_data, extra_data, extra_data_size);
		decode->decoder->extradata = decode->extra_data;
		decode->decoder->extradata_size = extra_data_size;
		decode->decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
	}

	ret = avcodec_open2(decode->decoder, decode->codec, NULL);
	if (ret < 0) {
		ffmpeg_decode_free(decode);
		return ret;
	}

	if (decode->codec->capabilities & CODEC_CAP_TRUNC)
		decode->decoder->flags |= CODEC_FLAG_TRUNC;

	return 0;
}

void ffmpeg_decode_free(struct ffmpeg_decode *decode)
{
	if (decode->hw_frame)
		av_free(decode->hw_frame);

	if (decode->decoder) {
		avcodec_close(decode->decoder);
		av_free(decode->decoder);
	}

	if (decode->frame)
		av_free(decode->frame);

	if (decode->packet_buffer)
		bfree(decode->packet_buffer);

	if (decode->extra_data)
		bfree(decode->extra_data);

	memset(decode, 0, sizeof(*decode));
}

static inline enum video_format convert_pixel_format(int f)
{
	switch (f) {
	case AV_PIX_FMT_NONE:
		return VIDEO_FORMAT_NONE;
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUYV422:
		return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_UYVY422:
		return VIDEO_FORMAT_UYVY;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_RGBA:
		return VIDEO_FORMAT_RGBA;
	case AV_PIX_FMT_BGRA:
		return VIDEO_FORMAT_BGRA;
	case AV_PIX_FMT_BGR0:
		return VIDEO_FORMAT_BGRX;
	default:;
	}

	return VIDEO_FORMAT_NONE;
}

static inline enum audio_format convert_sample_format(int f)
{
	switch (f) {
	case AV_SAMPLE_FMT_U8:
		return AUDIO_FORMAT_U8BIT;
	case AV_SAMPLE_FMT_S16:
		return AUDIO_FORMAT_16BIT;
	case AV_SAMPLE_FMT_S32:
		return AUDIO_FORMAT_32BIT;
	case AV_SAMPLE_FMT_FLT:
		return AUDIO_FORMAT_FLOAT;
	case AV_SAMPLE_FMT_U8P:
		return AUDIO_FORMAT_U8BIT_PLANAR;
	case AV_SAMPLE_FMT_S16P:
		return AUDIO_FORMAT_16BIT_PLANAR;
	case AV_SAMPLE_FMT_S32P:
		return AUDIO_FORMAT_32BIT_PLANAR;
	case AV_SAMPLE_FMT_FLTP:
		return AUDIO_FORMAT_FLOAT_PLANAR;
	default:;
	}

	return AUDIO_FORMAT_UNKNOWN;
}

static inline enum speaker_layout convert_speaker_layout(uint8_t channels)
{
	switch (channels) {
	case 0:
		return SPEAKERS_UNKNOWN;
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

static inline void copy_data(struct ffmpeg_decode *decode, uint8_t *data,
			     size_t size)
{
	size_t new_size = size + INPUT_BUFFER_PADDING_SIZE;

	if (decode->packet_size < new_size) {
		decode->packet_buffer =
			brealloc(decode->packet_buffer, new_size);
		decode->packet_size = new_size;
	}

	memset(decode->packet_buffer + size, 0, INPUT_BUFFER_PADDING_SIZE);
	memcpy(decode->packet_buffer, data, size);
}

bool ffmpeg_decode_audio(struct ffmpeg_decode *decode, uint8_t *data,
			 size_t size, bool copy, struct obs_source_audio *audio,
			 bool *got_output)
{
	AVPacket packet = {0};
	int got_frame = false;
	int ret = 0;

	*got_output = false;

	if (copy)
		copy_data(decode, data, size);

	av_init_packet(&packet);
	packet.data = copy ? decode->packet_buffer : data;
	packet.size = (int)size;

	if (!decode->frame) {
		decode->frame = av_frame_alloc();
		if (!decode->frame)
			return false;
	}

	if (data && size)
		ret = avcodec_send_packet(decode->decoder, &packet);
	if (ret == 0)
		ret = avcodec_receive_frame(decode->decoder, decode->frame);

	got_frame = (ret == 0);

	if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
		ret = 0;

	if (ret < 0)
		return false;
	else if (!got_frame)
		return true;

	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		audio->data[i] = decode->frame->data[i];

	audio->samples_per_sec = decode->frame->sample_rate;
	audio->format = convert_sample_format(decode->frame->format);
	audio->speakers =
		convert_speaker_layout((uint8_t)decode->decoder->channels);

	audio->frames = decode->frame->nb_samples;

	if (audio->format == AUDIO_FORMAT_UNKNOWN)
		return false;

	*got_output = true;
	return true;
}

bool ffmpeg_decode_video(struct ffmpeg_decode *decode, uint8_t *data,
			 size_t size, bool copy, long long *ts,
			 enum video_range_type range,
			 struct obs_source_frame2 *frame, bool *got_output)
{
	AVPacket packet = {0};
	int got_frame = false;
	AVFrame *out_frame;
	int ret;

	*got_output = false;

	if (copy)
		copy_data(decode, data, size);

	av_init_packet(&packet);
	packet.data = copy ? decode->packet_buffer : data;
	packet.size = (int)size;
	packet.pts = *ts;

	if (decode->codec->id == AV_CODEC_ID_H264 &&
	    obs_avc_keyframe(data, size))
		packet.flags |= AV_PKT_FLAG_KEY;

	if (!decode->frame) {
		decode->frame = av_frame_alloc();
		if (!decode->frame)
			return false;

		if (decode->hw && !decode->hw_frame) {
			decode->hw_frame = av_frame_alloc();
			if (!decode->hw_frame)
				return false;
		}
	}

	out_frame = decode->hw ? decode->hw_frame : decode->frame;

	ret = avcodec_send_packet(decode->decoder, &packet);
	if (ret == 0) {
		ret = avcodec_receive_frame(decode->decoder, out_frame);
	}

	got_frame = (ret == 0);

	if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
		ret = 0;

	if (ret < 0)
		return false;
	else if (!got_frame)
		return true;

#ifdef USE_NEW_HARDWARE_CODEC_METHOD
	if (got_frame && decode->hw) {
		ret = av_hwframe_transfer_data(decode->frame, out_frame, 0);
		if (ret < 0) {
			return false;
		}
	}
#endif

	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		frame->data[i] = decode->frame->data[i];
		frame->linesize[i] = decode->frame->linesize[i];
	}

	frame->format = convert_pixel_format(decode->frame->format);

	if (range == VIDEO_RANGE_DEFAULT) {
		range = (decode->frame->color_range == AVCOL_RANGE_JPEG)
				? VIDEO_RANGE_FULL
				: VIDEO_RANGE_PARTIAL;
	}

	if (range != frame->range) {
		const bool success = video_format_get_parameters(
			VIDEO_CS_601, range, frame->color_matrix,
			frame->color_range_min, frame->color_range_max);
		if (!success) {
			plog(LOG_ERROR,
			     "Failed to get video format "
			     "parameters for video format %u",
			     VIDEO_CS_601);
			return false;
		}

		frame->range = range;
	}

	*ts = decode->frame->pkt_pts;

	frame->width = decode->frame->width;
	frame->height = decode->frame->height;

	//PRISM/LiuHaibin/20200609/#3174/camera effect
	frame->flip = false;

	if (frame->format == VIDEO_FORMAT_NONE)
		return false;

	*got_output = true;
	return true;
}

void ffmpeg_decode_flush(struct ffmpeg_decode* decode)
{
	avcodec_flush_buffers(decode->decoder);
}
