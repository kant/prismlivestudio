/******************************************************************************
    Copyright (C) 2016 by Hugh Bailey <obs.jim@gmail.com>

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

#include <util/darray.h>
#include <util/dstr.h>
#include <util/base.h>
#include <media-io/video-io.h>
#include <obs-module.h>
#include <obs-avc.h>
#include <obs-hevc.h>

#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavformat/avformat.h>

#include "obs-ffmpeg-formats.h"
#include "jim-nvenc.h"

#define do_log(level, format, ...)                   \
	plog(level, "[NVENC encoder: '%s'] " format, \
	     obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

struct nvenc_encoder {
	obs_encoder_t *encoder;

	AVCodec *nvenc;
	AVCodecContext *context;

	AVFrame *vframe;

	DARRAY(uint8_t) buffer;

	uint8_t *header;
	size_t header_size;

	uint8_t *sei;
	size_t sei_size;

	int height;
	bool first_packet;
	bool initialized;

	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	bool is_h265_encoder;
};

static const char *nvenc_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "NVIDIA NVENC H.264";
}

//PRISM/Wangshaohui/20201230/#3786/support HEVC
static const char *nvenc_hevc_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "NVIDIA NVENC H.265";
}

static inline bool valid_format(enum video_format format)
{
	return format == VIDEO_FORMAT_I420 || format == VIDEO_FORMAT_NV12 ||
	       format == VIDEO_FORMAT_I444;
}

static void nvenc_video_info(void *data, struct video_scale_info *info)
{
	struct nvenc_encoder *enc = data;
	enum video_format pref_format;

	pref_format = obs_encoder_get_preferred_video_format(enc->encoder);

	if (!valid_format(pref_format)) {
		pref_format = valid_format(info->format) ? info->format
							 : VIDEO_FORMAT_NV12;
	}

	info->format = pref_format;
}

static bool nvenc_init_codec(struct nvenc_encoder *enc)
{
	int ret;

	ret = avcodec_open2(enc->context, enc->nvenc, NULL);
	if (ret < 0) {
		warn("Failed to open NVENC codec: %s", av_err2str(ret));
		return false;
	}

	enc->vframe = av_frame_alloc();
	if (!enc->vframe) {
		warn("Failed to allocate video frame");
		return false;
	}

	enc->vframe->format = enc->context->pix_fmt;
	enc->vframe->width = enc->context->width;
	enc->vframe->height = enc->context->height;
	enc->vframe->colorspace = enc->context->colorspace;
	enc->vframe->color_range = enc->context->color_range;

	ret = av_frame_get_buffer(enc->vframe, base_get_alignment());
	if (ret < 0) {
		warn("Failed to allocate vframe: %s", av_err2str(ret));
		return false;
	}

	enc->initialized = true;
	return true;
}

enum RC_MODE { RC_MODE_CBR, RC_MODE_VBR, RC_MODE_CQP, RC_MODE_LOSSLESS };

static bool nvenc_update(void *data, obs_data_t *settings)
{
	struct nvenc_encoder *enc = data;

	const char *rc = obs_data_get_string(settings, "rate_control");
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	int cqp = (int)obs_data_get_int(settings, "cqp");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	const char *preset = obs_data_get_string(settings, "preset");
	const char *profile = obs_data_get_string(settings, "profile");
	int gpu = (int)obs_data_get_int(settings, "gpu");
	bool cbr_override = obs_data_get_bool(settings, "cbr");
	int bf = (int)obs_data_get_int(settings, "bf");

	//PRISM/Wangshaohui/20210311/#7016/remove b frames
	if (enc->is_h265_encoder) {
		bf = 0;
	}

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	struct video_scale_info info;

	/* XXX: "cbr" setting has been deprecated */
	if (cbr_override) {
		warn("\"cbr\" setting has been deprecated for all encoders!  "
		     "Please set \"rate_control\" to \"CBR\" instead.  "
		     "Forcing CBR mode.  "
		     "(Note to all: this is why you shouldn't use strings for "
		     "common settings)");
		rc = "CBR";
	}

	info.format = voi->format;
	info.colorspace = voi->colorspace;
	info.range = voi->range;

	bool twopass = false;

	if (astrcmpi(preset, "mq") == 0) {
		twopass = true;
		preset = "hq";
	}

	nvenc_video_info(enc, &info);
	av_opt_set_int(enc->context->priv_data, "cbr", false, 0);
	av_opt_set(enc->context->priv_data, "profile", profile, 0);
	av_opt_set(enc->context->priv_data, "preset", preset, 0);

	if (astrcmpi(rc, "cqp") == 0) {
		bitrate = 0;
		enc->context->global_quality = cqp;

	} else if (astrcmpi(rc, "lossless") == 0) {
		bitrate = 0;
		cqp = 0;

		bool hp = (astrcmpi(preset, "hp") == 0 ||
			   astrcmpi(preset, "llhp") == 0);

		av_opt_set(enc->context->priv_data, "preset",
			   hp ? "losslesshp" : "lossless", 0);

	} else if (astrcmpi(rc, "vbr") != 0) { /* CBR by default */
		av_opt_set_int(enc->context->priv_data, "cbr", true, 0);
		enc->context->rc_max_rate = bitrate * 1000;
		enc->context->rc_min_rate = bitrate * 1000;
		cqp = 0;
	}

	av_opt_set(enc->context->priv_data, "level", "auto", 0);
	av_opt_set_int(enc->context->priv_data, "2pass", twopass, 0);
	av_opt_set_int(enc->context->priv_data, "gpu", gpu, 0);

	enc->context->bit_rate = bitrate * 1000;
	enc->context->rc_buffer_size = bitrate * 1000;
	enc->context->width = obs_encoder_get_width(enc->encoder);
	enc->context->height = obs_encoder_get_height(enc->encoder);
	enc->context->time_base = (AVRational){voi->fps_den, voi->fps_num};
	enc->context->pix_fmt = obs_to_ffmpeg_video_format(info.format);
	enc->context->colorspace = info.colorspace == VIDEO_CS_709
					   ? AVCOL_SPC_BT709
					   : AVCOL_SPC_BT470BG;
	enc->context->color_range = info.range == VIDEO_RANGE_FULL
					    ? AVCOL_RANGE_JPEG
					    : AVCOL_RANGE_MPEG;
	enc->context->max_b_frames = bf;

	//PRISM/LiuHaibin/20210803/#9045/add protection division operation
	if (keyint_sec)
		enc->context->gop_size =
			voi->fps_den ? keyint_sec * voi->fps_num / voi->fps_den
				     : keyint_sec * 30;
	else
		enc->context->gop_size = 250;

	enc->height = enc->context->height;

	info("settings:\n"
	     "\trate_control: %s\n"
	     "\tbitrate:      %d\n"
	     "\tcqp:          %d\n"
	     "\tkeyint:       %d\n"
	     "\tpreset:       %s\n"
	     "\tprofile:      %s\n"
	     "\twidth:        %d\n"
	     "\theight:       %d\n"
	     "\t2-pass:       %s\n"
	     "\tb-frames:     %d\n"
	     "\tGPU:          %d\n",
	     rc, bitrate, cqp, enc->context->gop_size, preset, profile,
	     enc->context->width, enc->context->height,
	     twopass ? "true" : "false", enc->context->max_b_frames, gpu);

	return nvenc_init_codec(enc);
}

static bool nvenc_reconfigure(void *data, obs_data_t *settings)
{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 19, 101)
	struct nvenc_encoder *enc = data;

	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool cbr = astrcmpi(rc, "CBR") == 0;
	bool vbr = astrcmpi(rc, "VBR") == 0;
	if (cbr || vbr) {
		enc->context->bit_rate = bitrate * 1000;
		enc->context->rc_max_rate = bitrate * 1000;
	}
#endif
	return true;
}

static void nvenc_destroy(void *data)
{
	struct nvenc_encoder *enc = data;

	if (enc->initialized) {
		AVPacket pkt = {0};
		int r_pkt = 1;

		while (r_pkt) {
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
			if (avcodec_receive_packet(enc->context, &pkt) < 0)
				break;
#else
			if (avcodec_encode_video2(enc->context, &pkt, NULL,
						  &r_pkt) < 0)
				break;
#endif

			if (r_pkt)
				av_packet_unref(&pkt);
		}
	}

	avcodec_close(enc->context);
	av_frame_unref(enc->vframe);
	av_frame_free(&enc->vframe);
	da_free(enc->buffer);
	bfree(enc->header);
	bfree(enc->sei);

	bfree(enc);
}

static void *nvenc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	struct nvenc_encoder *enc;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
	avcodec_register_all();
#endif

	enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;
	enc->nvenc = avcodec_find_encoder_by_name("h264_nvenc");
	if (!enc->nvenc)
		enc->nvenc = avcodec_find_encoder_by_name("nvenc_h264");
	enc->first_packet = true;

	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	enc->is_h265_encoder = false;

	plog(LOG_INFO, "---------------------------------");

	if (!enc->nvenc) {
		warn("Couldn't find encoder");
		goto fail;
	}

	enc->context = avcodec_alloc_context3(enc->nvenc);
	if (!enc->context) {
		warn("Failed to create codec context");
		goto fail;
	}

	if (!nvenc_update(enc, settings))
		goto fail;

	return enc;

fail:
	nvenc_destroy(enc);
	return NULL;
}

//PRISM/Wangshaohui/20201230/#3786/support HEVC
static void *nvenc_hevc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	struct nvenc_encoder *enc;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
	avcodec_register_all();
#endif

	enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;
	enc->nvenc = avcodec_find_encoder_by_name("hevc_nvenc");
	enc->first_packet = true;
	enc->is_h265_encoder = true;

	plog(LOG_INFO, "---------------------------------");

	if (!enc->nvenc) {
		warn("Couldn't find encoder");
		goto fail;
	}

	enc->context = avcodec_alloc_context3(enc->nvenc);
	if (!enc->context) {
		warn("Failed to create codec context");
		goto fail;
	}

	if (!nvenc_update(enc, settings))
		goto fail;

	return enc;

fail:
	nvenc_destroy(enc);
	return NULL;
}

static inline void copy_data(AVFrame *pic, const struct encoder_frame *frame,
			     int height, enum AVPixelFormat format)
{
	int h_chroma_shift, v_chroma_shift;
	av_pix_fmt_get_chroma_sub_sample(format, &h_chroma_shift,
					 &v_chroma_shift);
	for (int plane = 0; plane < MAX_AV_PLANES; plane++) {
		if (!frame->data[plane])
			continue;

		int frame_rowsize = (int)frame->linesize[plane];
		int pic_rowsize = pic->linesize[plane];
		int bytes = frame_rowsize < pic_rowsize ? frame_rowsize
							: pic_rowsize;
		int plane_height = height >> (plane ? v_chroma_shift : 0);

		for (int y = 0; y < plane_height; y++) {
			int pos_frame = y * frame_rowsize;
			int pos_pic = y * pic_rowsize;

			memcpy(pic->data[plane] + pos_pic,
			       frame->data[plane] + pos_frame, bytes);
		}
	}
}

static bool nvenc_encode(void *data, struct encoder_frame *frame,
			 struct encoder_packet *packet, bool *received_packet)
{
	struct nvenc_encoder *enc = data;
	AVPacket av_pkt = {0};
	int got_packet;
	int ret;

	av_init_packet(&av_pkt);

	copy_data(enc->vframe, frame, enc->height, enc->context->pix_fmt);

	enc->vframe->pts = frame->pts;
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
	ret = avcodec_send_frame(enc->context, enc->vframe);
	if (ret == 0)
		ret = avcodec_receive_packet(enc->context, &av_pkt);

	got_packet = (ret == 0);

	if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
		ret = 0;
#else
	ret = avcodec_encode_video2(enc->context, &av_pkt, enc->vframe,
				    &got_packet);
#endif
	if (ret < 0) {
		warn("nvenc_encode: Error encoding: %s", av_err2str(ret));
		return false;
	}

	if (got_packet && av_pkt.size) {
		if (enc->first_packet) {
			uint8_t *new_packet;
			size_t size;

			enc->first_packet = false;
			//PRISM/Wangshaohui/20201230/#3786/support HEVC
			if (enc->is_h265_encoder) {
				obs_extract_hevc_headers(
					av_pkt.data, av_pkt.size, &new_packet,
					&size, &enc->header, &enc->header_size,
					&enc->sei, &enc->sei_size);
			} else {
				obs_extract_avc_headers(
					av_pkt.data, av_pkt.size, &new_packet,
					&size, &enc->header, &enc->header_size,
					&enc->sei, &enc->sei_size);
			}

			da_copy_array(enc->buffer, new_packet, size);
			bfree(new_packet);
		} else {
			da_copy_array(enc->buffer, av_pkt.data, av_pkt.size);
		}

		packet->pts = av_pkt.pts;
		packet->dts = av_pkt.dts;
		packet->data = enc->buffer.array;
		packet->size = enc->buffer.num;
		packet->type = OBS_ENCODER_VIDEO;

		//PRISM/Wangshaohui/20201230/#3786/support HEVC
		if (enc->is_h265_encoder) {
			packet->keyframe =
				obs_hevc_keyframe(packet->data, packet->size);
		} else {
			packet->keyframe =
				obs_avc_keyframe(packet->data, packet->size);
		}
		*received_packet = true;
	} else {
		*received_packet = false;
	}

	av_packet_unref(&av_pkt);
	return true;
}

void nvenc_defaults(obs_data_t *settings)
{
	//PRISM/LiuHaibin/20200221/#/change default value : bitrate,max_bitrate,keyint_sec,bf
	obs_data_set_default_int(settings, "bitrate", 6000);
	obs_data_set_default_int(settings, "max_bitrate", 9000);
	obs_data_set_default_int(settings, "keyint_sec", 1);
	obs_data_set_default_int(settings, "cqp", 20);
	obs_data_set_default_string(settings, "rate_control", "CBR");
	obs_data_set_default_string(settings, "preset", "hq");
	obs_data_set_default_string(settings, "profile", "high");
	obs_data_set_default_bool(settings, "psycho_aq", true);
	obs_data_set_default_int(settings, "gpu", 0);
	obs_data_set_default_int(settings, "bf", 0);
}

//PRISM/Wangshaohui/20201230/#3786/support HEVC
void nvenc_hevc_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", HEVC_DEFAULT_BITRATT);
	obs_data_set_default_int(settings, "max_bitrate", HEVC_DEFAULT_BITRATT);
	obs_data_set_default_int(settings, "keyint_sec", HEVC_KEYFRAME_SEC);
	obs_data_set_default_int(settings, "cqp", HEVC_DEFAULT_CQP);
	obs_data_set_default_string(settings, "rate_control", "CBR");
	obs_data_set_default_string(settings, "preset", "hq");
	obs_data_set_default_string(settings, "profile", "main");
	obs_data_set_default_bool(settings, "psycho_aq", true);
	obs_data_set_default_int(settings, "gpu", 0);
	obs_data_set_default_int(settings, "bf", 0);
}

static bool rate_control_modified(obs_properties_t *ppts, obs_property_t *p,
				  obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool cqp = astrcmpi(rc, "CQP") == 0;
	bool vbr = astrcmpi(rc, "VBR") == 0;
	bool lossless = astrcmpi(rc, "lossless") == 0;
	size_t count;

	p = obs_properties_get(ppts, "bitrate");
	obs_property_set_visible(p, !cqp && !lossless);
	p = obs_properties_get(ppts, "max_bitrate");
	obs_property_set_visible(p, vbr);
	p = obs_properties_get(ppts, "cqp");
	obs_property_set_visible(p, cqp);

	p = obs_properties_get(ppts, "preset");
	count = obs_property_list_item_count(p);

	for (size_t i = 0; i < count; i++) {
		bool compatible = (i == 0 || i == 3);
		obs_property_list_item_disable(p, i, lossless && !compatible);
	}

	return true;
}

//PRISM/Wangshaohui/20201230/#3786/support HEVC
// defined in ffmpeg
static const char *const nvenc_hevc_profile_list[] = {
	"main",
	"main10",
	"rext",
	NULL,
};

//PRISM/Wangshaohui/20210311/#7022/limit max gpu
int nvenc_get_max_gpu()
{
	int nvidia_count = 0;
	obs_enter_graphics();
	gs_adapters_info_t *adapterInfo = gs_adapter_get_info();
	if (adapterInfo) {
		for (size_t i = 0; i < adapterInfo->adapters.num; i++) {
			struct adapter_info info =
				adapterInfo->adapters.array[i];
			if (wstrstri(info.description, L"nvidia") &&
			    !is_blacklisted(info.description)) {
				++nvidia_count;
			}
		}
	}
	obs_leave_graphics();

	int max_gpu = 0;
	if (nvidia_count > 1) {
		max_gpu = nvidia_count - 1;
	}

	return max_gpu;
}

obs_properties_t *nvenc_properties_internal(bool ffmpeg, bool is_h265)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "rate_control",
				    obs_module_text("RateControl"),
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "CBR", "CBR");
	obs_property_list_add_string(p, "CQP", "CQP");
	obs_property_list_add_string(p, "VBR", "VBR");
	obs_property_list_add_string(p, obs_module_text("Lossless"),
				     "lossless");

	obs_property_set_modified_callback(p, rate_control_modified);

	//PRISM/LiuHaibin/20200328/#/change default value
	p = obs_properties_add_int(props, "bitrate", obs_module_text("Bitrate"),
				   50, 60000, 50);
	obs_property_int_set_suffix(p, " Kbps");
	//PRISM/LiuHaibin/20200328/#/change default value
	p = obs_properties_add_int(props, "max_bitrate",
				   obs_module_text("MaxBitrate"), 50, 60000,
				   50);
	obs_property_int_set_suffix(p, " Kbps");

	obs_properties_add_int(props, "cqp", obs_module_text("NVENC.CQLevel"),
			       1, 30, 1);

	obs_properties_add_int(props, "keyint_sec",
			       obs_module_text("KeyframeIntervalSec"), 0, 10,
			       1);

	p = obs_properties_add_list(props, "preset", obs_module_text("Preset"),
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);

#define add_preset(val)                                                       \
	obs_property_list_add_string(p, obs_module_text("NVENC.Preset." val), \
				     val)
	add_preset("mq");
	add_preset("hq");
	add_preset("default");
	add_preset("hp");
	add_preset("ll");
	add_preset("llhq");
	add_preset("llhp");
#undef add_preset

	p = obs_properties_add_list(props, "profile",
				    obs_module_text("Profile"),
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);

	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	if (is_h265) {
		for (size_t i = 0; nvenc_hevc_profile_list[i] != NULL; i++) {
			obs_property_list_add_string(
				p, nvenc_hevc_profile_list[i],
				nvenc_hevc_profile_list[i]);
		}
	} else {
#define add_profile(val) obs_property_list_add_string(p, val, val)
		add_profile("high");
		add_profile("main");
		add_profile("baseline");
#undef add_profile
	}

	if (!ffmpeg) {
		p = obs_properties_add_bool(props, "lookahead",
					    obs_module_text("NVENC.LookAhead"));
		obs_property_set_long_description(
			p, obs_module_text("NVENC.LookAhead.ToolTip"));

		p = obs_properties_add_bool(
			props, "psycho_aq",
			obs_module_text("NVENC.PsychoVisualTuning"));
		obs_property_set_long_description(
			p, obs_module_text("NVENC.PsychoVisualTuning.ToolTip"));
	}

	//PRISM/Wangshaohui/20210311/#7022/limit max gpu
	int min = 0;
	int max = nvenc_get_max_gpu();
	obs_property_t *gpu_property = obs_properties_add_int(
		props, "gpu", obs_module_text("GPU"), min, max, 1);
	obs_property_set_enabled(gpu_property, min < max);

	//PRISM/Wangshaohui/20210311/#7016/remove b frames
	if (!is_h265) {
		obs_properties_add_int(props, "bf", obs_module_text("BFrames"),
				       0, 4, 1);
	}

	return props;
}

obs_properties_t *nvenc_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	return nvenc_properties_internal(false, false);
}

obs_properties_t *nvenc_properties_ffmpeg(void *unused)
{
	UNUSED_PARAMETER(unused);
	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	return nvenc_properties_internal(true, false);
}

//PRISM/Wangshaohui/20201230/#3786/support HEVC
obs_properties_t *nvenc_properties_hevc(void *unused)
{
	UNUSED_PARAMETER(unused);
	return nvenc_properties_internal(false, true);
}

//PRISM/Wangshaohui/20201230/#3786/support HEVC
obs_properties_t *nvenc_properties_ffmpeg_hevc(void *unused)
{
	UNUSED_PARAMETER(unused);
	return nvenc_properties_internal(true, true);
}

static bool nvenc_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct nvenc_encoder *enc = data;

	*extra_data = enc->header;
	*size = enc->header_size;
	return true;
}

static bool nvenc_sei_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct nvenc_encoder *enc = data;

	*extra_data = enc->sei;
	*size = enc->sei_size;
	return true;
}

//PRISM/ZengQin/20210528/#none/get encoder props params
static obs_data_t *nvenc_props_prams(void *data)
{
	if (!data)
		return NULL;

	struct nvenc_encoder *enc = data;
	obs_data_t *settings = obs_encoder_get_settings(enc->encoder);
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	const char *rc = obs_data_get_string(settings, "rate_control");
	int cqp = (int)obs_data_get_int(settings, "cqp");
	const char *preset = obs_data_get_string(settings, "preset");
	const char *profile = obs_data_get_string(settings, "profile");
	int gpu = (int)obs_data_get_int(settings, "gpu");
	bool cbr_override = obs_data_get_bool(settings, "cbr");
	obs_data_release(settings);

	if (cbr_override) {
		rc = "CBR";
	}

	if (astrcmpi(rc, "cqp") == 0) {
		bitrate = 0;

	} else if (astrcmpi(rc, "lossless") == 0) {
		bitrate = 0;
		cqp = 0;
	} else if (astrcmpi(rc, "vbr") != 0) { /* CBR by default */
		cqp = 0;
	}

	bool twopass = false;
	if (astrcmpi(preset, "mq") == 0) {
		twopass = true;
		preset = "hq";
	}

	obs_data_t *params = obs_data_create();
	obs_data_set_string(params, "rate_control", rc);
	obs_data_set_int(params, "bitrate", bitrate);
	obs_data_set_int(params, "cqp", cqp);
	obs_data_set_int(params, "keyint", enc->context->gop_size);
	obs_data_set_string(params, "preset", preset);
	obs_data_set_string(params, "profile", profile);
	obs_data_set_int(params, "width", enc->context->width);
	obs_data_set_int(params, "height", enc->context->height);
	obs_data_set_string(params, "2-pass", twopass ? "true" : "false");
	obs_data_set_int(params, "bframes", enc->context->max_b_frames);
	obs_data_set_int(params, "GPU", gpu);
	return params;
}

struct obs_encoder_info nvenc_encoder_info = {
	.id = "ffmpeg_nvenc",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.get_name = nvenc_getname,
	.create = nvenc_create,
	.destroy = nvenc_destroy,
	.encode = nvenc_encode,
	.update = nvenc_reconfigure,
	.get_defaults = nvenc_defaults,
	.get_properties = nvenc_properties_ffmpeg,
	.get_extra_data = nvenc_extra_data,
	.get_sei_data = nvenc_sei_data,
	.get_video_info = nvenc_video_info,
#ifdef _WIN32
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_INTERNAL,
#else
	.caps = OBS_ENCODER_CAP_DYN_BITRATE,
#endif
	//PRISM/ZengQin/20210528/#none/get encoder props params
	.props_params = nvenc_props_prams,
};

//PRISM/Wangshaohui/20201230/#3786/support HEVC
struct obs_encoder_info nvenc_hevc_encoder_info = {
	.id = "ffmpeg_nvenc_hevc",
	.type = OBS_ENCODER_VIDEO,
	.codec = "hevc",
	.get_name = nvenc_hevc_getname,
	.create = nvenc_hevc_create,
	.destroy = nvenc_destroy,
	.encode = nvenc_encode,
	.update = nvenc_reconfigure,
	.get_defaults = nvenc_hevc_defaults,
	.get_properties = nvenc_properties_ffmpeg_hevc,
	.get_extra_data = nvenc_extra_data,
	.get_sei_data = nvenc_sei_data,
	.get_video_info = nvenc_video_info,
#ifdef _WIN32
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_INTERNAL,
#else
	.caps = OBS_ENCODER_CAP_DYN_BITRATE,
#endif
	//PRISM/ZengQin/20210528/#none/get encoder props params
	.props_params = nvenc_props_prams,
};
