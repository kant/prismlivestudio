#include "jim-nvenc.h"
#include <util/circlebuf.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <obs-avc.h>
#include <obs-hevc.h>
#define INITGUID
#include <dxgi.h>
#include <d3d11.h>
#include <d3d11_1.h>

/* ========================================================================= */

#define EXTRA_BUFFERS 5

#define do_log(level, format, ...)               \
	plog(level, "[jim-nvenc: '%s'] " format, \
	     obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR, format, ##__VA_ARGS__)
#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

#define error_hr(msg) error("%s: %s: 0x%08lX", __FUNCTION__, msg, (uint32_t)hr);

struct nv_bitstream;
struct nv_texture;

struct handle_tex {
	uint32_t handle;
	ID3D11Texture2D *tex;
	IDXGIKeyedMutex *km;
};

/* ------------------------------------------------------------------------- */
/* Main Implementation Structure                                             */

struct nvenc_data {
	obs_encoder_t *encoder;

	void *session;
	NV_ENC_INITIALIZE_PARAMS params;
	NV_ENC_CONFIG config;
	size_t buf_count;
	size_t output_delay;
	size_t buffers_queued;
	size_t next_bitstream;
	size_t cur_bitstream;
	bool encode_started;
	bool first_packet;
	bool can_change_bitrate;
	bool bframes;

	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	bool is_h265_encoder;

	DARRAY(struct nv_bitstream) bitstreams;
	DARRAY(struct nv_texture) textures;
	DARRAY(struct handle_tex) input_textures;
	struct circlebuf dts_list;

	DARRAY(uint8_t) packet_data;
	int64_t packet_pts;
	bool packet_keyframe;

	ID3D11Device *device;
	ID3D11DeviceContext *context;

	uint32_t cx;
	uint32_t cy;

	uint8_t *header;
	size_t header_size;

	uint8_t *sei;
	size_t sei_size;
};

/* ------------------------------------------------------------------------- */
/* Bitstream Buffer                                                          */

struct nv_bitstream {
	void *ptr;
	HANDLE event;
};

static inline bool nv_failed(struct nvenc_data *enc, NVENCSTATUS err,
			     const char *func, const char *call)
{
	if (err == NV_ENC_SUCCESS)
		return false;

	error("%s: %s failed: %d (%s)", func, call, (int)err,
	      nv_error_name(err));
	return true;
}

#define NV_FAILED(x) nv_failed(enc, x, __FUNCTION__, #x)

static bool nv_bitstream_init(struct nvenc_data *enc, struct nv_bitstream *bs)
{
	NV_ENC_CREATE_BITSTREAM_BUFFER buf = {
		NV_ENC_CREATE_BITSTREAM_BUFFER_VER};
	NV_ENC_EVENT_PARAMS params = {NV_ENC_EVENT_PARAMS_VER};
	HANDLE event = NULL;

	if (NV_FAILED(nv.nvEncCreateBitstreamBuffer(enc->session, &buf))) {
		return false;
	}

	event = CreateEvent(NULL, true, true, NULL);
	if (!event) {
		error("%s: %s", __FUNCTION__, "Failed to create event");
		goto fail;
	}

	params.completionEvent = event;
	if (NV_FAILED(nv.nvEncRegisterAsyncEvent(enc->session, &params))) {
		goto fail;
	}

	bs->ptr = buf.bitstreamBuffer;
	bs->event = event;
	return true;

fail:
	if (event) {
		CloseHandle(event);
	}
	if (buf.bitstreamBuffer) {
		nv.nvEncDestroyBitstreamBuffer(enc->session,
					       buf.bitstreamBuffer);
	}
	return false;
}

static void nv_bitstream_free(struct nvenc_data *enc, struct nv_bitstream *bs)
{
	if (bs->ptr) {
		nv.nvEncDestroyBitstreamBuffer(enc->session, bs->ptr);

		NV_ENC_EVENT_PARAMS params = {NV_ENC_EVENT_PARAMS_VER};
		params.completionEvent = bs->event;
		nv.nvEncUnregisterAsyncEvent(enc->session, &params);
		CloseHandle(bs->event);
	}
}

/* ------------------------------------------------------------------------- */
/* Texture Resource                                                          */

struct nv_texture {
	void *res;
	ID3D11Texture2D *tex;
	void *mapped_res;
};

static bool nv_texture_init(struct nvenc_data *enc, struct nv_texture *nvtex)
{
	ID3D11Device *device = enc->device;
	ID3D11Texture2D *tex;
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = {0};
	desc.Width = enc->cx;
	desc.Height = enc->cy;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_NV12;
	desc.SampleDesc.Count = 1;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;

	hr = device->lpVtbl->CreateTexture2D(device, &desc, NULL, &tex);
	if (FAILED(hr)) {
		error_hr("Failed to create texture");
		return false;
	}

	tex->lpVtbl->SetEvictionPriority(tex, DXGI_RESOURCE_PRIORITY_MAXIMUM);

	NV_ENC_REGISTER_RESOURCE res = {NV_ENC_REGISTER_RESOURCE_VER};
	res.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
	res.resourceToRegister = tex;
	res.width = enc->cx;
	res.height = enc->cy;
	res.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;

	if (NV_FAILED(nv.nvEncRegisterResource(enc->session, &res))) {
		tex->lpVtbl->Release(tex);
		return false;
	}

	nvtex->res = res.registeredResource;
	nvtex->tex = tex;
	return true;
}

static void nv_texture_free(struct nvenc_data *enc, struct nv_texture *nvtex)
{
	if (nvtex->res) {
		if (nvtex->mapped_res) {
			nv.nvEncUnmapInputResource(enc->session,
						   nvtex->mapped_res);
		}
		nv.nvEncUnregisterResource(enc->session, nvtex->res);
		nvtex->tex->lpVtbl->Release(nvtex->tex);
	}
}

/* ------------------------------------------------------------------------- */
/* Implementation                                                            */

static const char *nvenc_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "NVIDIA NVENC H.264 (new)";
}

//PRISM/Wangshaohui/20201230/#3786/support HEVC
static const char *nvenc_hevc_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "NVIDIA NVENC H.265 (new)";
}

//PRISM/Wangshaohui/20201230/#3786/support HEVC
static inline int nv_get_cap(struct nvenc_data *enc, NV_ENC_CAPS cap, GUID guid)
{
	if (!enc->session)
		return 0;

	NV_ENC_CAPS_PARAM param = {NV_ENC_CAPS_PARAM_VER};
	int v;

	param.capsToQuery = cap;
	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	nv.nvEncGetEncodeCaps(enc->session, guid, &param, &v);
	return v;
}

static bool nvenc_update(void *data, obs_data_t *settings)
{
	struct nvenc_data *enc = data;

	/* Only support reconfiguration of CBR bitrate */
	if (enc->can_change_bitrate) {
		int bitrate = (int)obs_data_get_int(settings, "bitrate");

		enc->config.rcParams.averageBitRate = bitrate * 1000;
		enc->config.rcParams.maxBitRate = bitrate * 1000;

		NV_ENC_RECONFIGURE_PARAMS params = {0};
		params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
		params.reInitEncodeParams = enc->params;
		params.resetEncoder = 1;
		params.forceIDR = 1;

		if (NV_FAILED(nv.nvEncReconfigureEncoder(enc->session,
							 &params))) {
			return false;
		}
	}

	return true;
}

static HANDLE get_lib(struct nvenc_data *enc, const char *lib)
{
	HMODULE mod = GetModuleHandleA(lib);
	if (mod)
		return mod;

	mod = LoadLibraryA(lib);
	if (!mod)
		error("Failed to load %s", lib);
	return mod;
}

typedef HRESULT(WINAPI *CREATEDXGIFACTORY1PROC)(REFIID, void **);

static bool init_d3d11(struct nvenc_data *enc, obs_data_t *settings)
{
	HMODULE dxgi = get_lib(enc, "DXGI.dll");
	HMODULE d3d11 = get_lib(enc, "D3D11.dll");
	CREATEDXGIFACTORY1PROC create_dxgi;
	PFN_D3D11_CREATE_DEVICE create_device;
	IDXGIFactory1 *factory;
	IDXGIAdapter *adapter;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	HRESULT hr;

	if (!dxgi || !d3d11) {
		return false;
	}

	create_dxgi = (CREATEDXGIFACTORY1PROC)GetProcAddress(
		dxgi, "CreateDXGIFactory1");
	create_device = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(
		d3d11, "D3D11CreateDevice");

	if (!create_dxgi || !create_device) {
		error("Failed to load D3D11/DXGI procedures");
		return false;
	}

	hr = create_dxgi(&IID_IDXGIFactory1, &factory);
	if (FAILED(hr)) {
		error_hr("CreateDXGIFactory1 failed");
		return false;
	}

	hr = factory->lpVtbl->EnumAdapters(factory, 0, &adapter);
	factory->lpVtbl->Release(factory);
	if (FAILED(hr)) {
		error_hr("EnumAdapters failed");
		return false;
	}

	hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
			   D3D11_SDK_VERSION, &device, NULL, &context);
	adapter->lpVtbl->Release(adapter);
	if (FAILED(hr)) {
		error_hr("D3D11CreateDevice failed");
		return false;
	}

	enc->device = device;
	enc->context = context;
	return true;
}

static bool init_session(struct nvenc_data *enc)
{
	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {
		NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
	params.device = enc->device;
	params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
	params.apiVersion = NVENCAPI_VERSION;

	if (NV_FAILED(nv.nvEncOpenEncodeSessionEx(&params, &enc->session))) {
		return false;
	}
	return true;
}

static bool init_encoder(struct nvenc_data *enc, obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	int max_bitrate = (int)obs_data_get_int(settings, "max_bitrate");
	int cqp = (int)obs_data_get_int(settings, "cqp");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	const char *preset = obs_data_get_string(settings, "preset");
	const char *profile = obs_data_get_string(settings, "profile");
	bool psycho_aq = obs_data_get_bool(settings, "psycho_aq");
	bool lookahead = obs_data_get_bool(settings, "lookahead");
	int bf = (int)obs_data_get_int(settings, "bf");
	bool vbr = astrcmpi(rc, "VBR") == 0;
	NVENCSTATUS err;

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	GUID codec_guid = NV_ENC_CODEC_H264_GUID;

	enc->cx = voi->width;
	enc->cy = voi->height;

	/* -------------------------- */
	/* get preset                 */

	GUID nv_preset = NV_ENC_PRESET_DEFAULT_GUID;
	bool twopass = false;
	bool hp = false;
	bool ll = false;

	if (astrcmpi(preset, "hq") == 0) {
		nv_preset = NV_ENC_PRESET_HQ_GUID;

	} else if (astrcmpi(preset, "mq") == 0) {
		nv_preset = NV_ENC_PRESET_HQ_GUID;
		twopass = true;

	} else if (astrcmpi(preset, "hp") == 0) {
		nv_preset = NV_ENC_PRESET_HP_GUID;
		hp = true;

	} else if (astrcmpi(preset, "ll") == 0) {
		nv_preset = NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID;
		ll = true;

	} else if (astrcmpi(preset, "llhq") == 0) {
		nv_preset = NV_ENC_PRESET_LOW_LATENCY_HQ_GUID;
		ll = true;

	} else if (astrcmpi(preset, "llhp") == 0) {
		nv_preset = NV_ENC_PRESET_LOW_LATENCY_HP_GUID;
		hp = true;
		ll = true;
	}

	if (astrcmpi(rc, "lossless") == 0) {
		nv_preset = hp ? NV_ENC_PRESET_LOSSLESS_HP_GUID
			       : NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID;
	}

	/* -------------------------- */
	/* get preset default config  */

	NV_ENC_PRESET_CONFIG preset_config = {NV_ENC_PRESET_CONFIG_VER,
					      {NV_ENC_CONFIG_VER}};

	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	err = nv.nvEncGetEncodePresetConfig(enc->session, codec_guid, nv_preset,
					    &preset_config);
	if (nv_failed(enc, err, __FUNCTION__, "nvEncGetEncodePresetConfig")) {
		return false;
	}

	/* -------------------------- */
	/* main configuration         */

	enc->config = preset_config.presetCfg;

	//PRISM/LiuHaibin/20210803/#9045/add protection division operation
	uint32_t gop_size = (keyint_sec && voi->fps_den)
				    ? keyint_sec * voi->fps_num / voi->fps_den
				    : 250;

	NV_ENC_INITIALIZE_PARAMS *params = &enc->params;
	NV_ENC_CONFIG *config = &enc->config;
	NV_ENC_CONFIG_H264 *h264_config = &config->encodeCodecConfig.h264Config;
	NV_ENC_CONFIG_H264_VUI_PARAMETERS *vui_params =
		&h264_config->h264VUIParameters;

	memset(params, 0, sizeof(*params));
	params->version = NV_ENC_INITIALIZE_PARAMS_VER;
	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	params->encodeGUID = codec_guid;
	params->presetGUID = nv_preset;
	params->encodeWidth = voi->width;
	params->encodeHeight = voi->height;
	params->darWidth = voi->width;
	params->darHeight = voi->height;
	params->frameRateNum = voi->fps_num;
	params->frameRateDen = voi->fps_den;
	params->enableEncodeAsync = 1;
	params->enablePTD = 1;
	params->encodeConfig = &enc->config;
	params->maxEncodeWidth = voi->width;
	params->maxEncodeHeight = voi->height;
	config->gopLength = gop_size;
	config->frameIntervalP = 1 + bf;
	h264_config->idrPeriod = gop_size;
	vui_params->videoSignalTypePresentFlag = 1;
	vui_params->videoFullRangeFlag = (voi->range == VIDEO_RANGE_FULL);
	vui_params->colourDescriptionPresentFlag = 1;
	vui_params->colourMatrix = (voi->colorspace == VIDEO_CS_709) ? 1 : 5;
	vui_params->colourPrimaries = 1;
	vui_params->transferCharacteristics = 1;

	enc->bframes = bf > 0;

	/* lookahead */
	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	if (lookahead &&
	    nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_LOOKAHEAD, codec_guid)) {
		config->rcParams.lookaheadDepth = 8;
		config->rcParams.enableLookahead = 1;
	} else {
		lookahead = false;
	}

	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	/* psycho aq */
	if (nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ, codec_guid)) {
		config->rcParams.enableAQ = psycho_aq;
		config->rcParams.enableTemporalAQ = psycho_aq;
	}

	/* -------------------------- */
	/* rate control               */
	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	enc->can_change_bitrate =
		nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE,
			   codec_guid) &&
		!lookahead;

	config->rcParams.rateControlMode = twopass ? NV_ENC_PARAMS_RC_VBR_HQ
						   : NV_ENC_PARAMS_RC_VBR;

	if (astrcmpi(rc, "cqp") == 0 || astrcmpi(rc, "lossless") == 0) {
		if (astrcmpi(rc, "lossless") == 0)
			cqp = 0;

		config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
		config->rcParams.constQP.qpInterP = cqp;
		config->rcParams.constQP.qpInterB = cqp;
		config->rcParams.constQP.qpIntra = cqp;
		enc->can_change_bitrate = false;

		bitrate = 0;
		max_bitrate = 0;

	} else if (astrcmpi(rc, "vbr") != 0) { /* CBR by default */
		h264_config->outputBufferingPeriodSEI = 1;
		config->rcParams.rateControlMode =
			twopass ? NV_ENC_PARAMS_RC_2_PASS_QUALITY
				: NV_ENC_PARAMS_RC_CBR;
	}

	h264_config->outputPictureTimingSEI = 1;
	config->rcParams.averageBitRate = bitrate * 1000;
	config->rcParams.maxBitRate = vbr ? max_bitrate * 1000 : bitrate * 1000;

	/* -------------------------- */
	/* profile                    */

	if (astrcmpi(profile, "main") == 0) {
		config->profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
	} else if (astrcmpi(profile, "baseline") == 0) {
		config->profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
	} else {
		config->profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
	}

	/* -------------------------- */
	/* initialize                 */

	if (NV_FAILED(nv.nvEncInitializeEncoder(enc->session, params))) {
		return false;
	}

	enc->buf_count = config->frameIntervalP +
			 config->rcParams.lookaheadDepth + EXTRA_BUFFERS;
	enc->output_delay = enc->buf_count - 1;

	info("jim-nvenc settings:\n"
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
	     "\tlookahead:    %s\n"
	     "\tpsycho_aq:    %s\n",
	     rc, bitrate, cqp, gop_size, preset, profile, enc->cx, enc->cy,
	     twopass ? "true" : "false", bf, lookahead ? "true" : "false",
	     psycho_aq ? "true" : "false");

	return true;
}

//PRISM/Wangshaohui/20201230/#3786/support HEVC
static bool init_encoder_hevc(struct nvenc_data *enc, obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	int max_bitrate = (int)obs_data_get_int(settings, "max_bitrate");
	int cqp = (int)obs_data_get_int(settings, "cqp");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	const char *preset = obs_data_get_string(settings, "preset");
	const char *profile = obs_data_get_string(settings, "profile");
	bool psycho_aq = obs_data_get_bool(settings, "psycho_aq");
	bool lookahead = obs_data_get_bool(settings, "lookahead");
	int bf = 0;
	bool vbr = astrcmpi(rc, "VBR") == 0;
	NVENCSTATUS err;

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	GUID codec_guid = NV_ENC_CODEC_HEVC_GUID;

	enc->cx = voi->width;
	enc->cy = voi->height;

	/* -------------------------- */
	/* get preset                 */

	GUID nv_preset = NV_ENC_PRESET_DEFAULT_GUID;
	bool twopass = false;
	bool hp = false;
	bool ll = false;

	if (astrcmpi(preset, "hq") == 0) {
		nv_preset = NV_ENC_PRESET_HQ_GUID;

	} else if (astrcmpi(preset, "mq") == 0) {
		nv_preset = NV_ENC_PRESET_HQ_GUID;
		twopass = true;

	} else if (astrcmpi(preset, "hp") == 0) {
		nv_preset = NV_ENC_PRESET_HP_GUID;
		hp = true;

	} else if (astrcmpi(preset, "ll") == 0) {
		nv_preset = NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID;
		ll = true;

	} else if (astrcmpi(preset, "llhq") == 0) {
		nv_preset = NV_ENC_PRESET_LOW_LATENCY_HQ_GUID;
		ll = true;

	} else if (astrcmpi(preset, "llhp") == 0) {
		nv_preset = NV_ENC_PRESET_LOW_LATENCY_HP_GUID;
		hp = true;
		ll = true;
	}

	if (astrcmpi(rc, "lossless") == 0) {
		nv_preset = hp ? NV_ENC_PRESET_LOSSLESS_HP_GUID
			       : NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID;
	}

	/* -------------------------- */
	/* get preset default config  */

	NV_ENC_PRESET_CONFIG preset_config = {NV_ENC_PRESET_CONFIG_VER,
					      {NV_ENC_CONFIG_VER}};

	err = nv.nvEncGetEncodePresetConfig(enc->session, codec_guid, nv_preset,
					    &preset_config);
	if (nv_failed(enc, err, __FUNCTION__, "nvEncGetEncodePresetConfig")) {
		return false;
	}

	/* -------------------------- */
	/* main configuration         */

	enc->config = preset_config.presetCfg;
	//PRISM/LiuHaibin/20210803/#9045/add protection division operation
	uint32_t gop_size = (keyint_sec && voi->fps_den)
				    ? keyint_sec * voi->fps_num / voi->fps_den
				    : 250;

	NV_ENC_INITIALIZE_PARAMS *params = &enc->params;
	NV_ENC_CONFIG *config = &enc->config;
	NV_ENC_CONFIG_HEVC *hevc_config = &config->encodeCodecConfig.hevcConfig;
	NV_ENC_CONFIG_HEVC_VUI_PARAMETERS *vui_params =
		&hevc_config->hevcVUIParameters;

	memset(params, 0, sizeof(*params));
	params->version = NV_ENC_INITIALIZE_PARAMS_VER;
	params->encodeGUID = codec_guid;
	params->presetGUID = nv_preset;
	params->encodeWidth = voi->width;
	params->encodeHeight = voi->height;
	params->darWidth = voi->width;
	params->darHeight = voi->height;
	params->frameRateNum = voi->fps_num;
	params->frameRateDen = voi->fps_den;
	params->enableEncodeAsync = 1;
	params->enablePTD = 1;
	params->encodeConfig = &enc->config;
	params->maxEncodeWidth = voi->width;
	params->maxEncodeHeight = voi->height;
	config->gopLength = gop_size;
	config->frameIntervalP = 1 + bf;
	hevc_config->idrPeriod = gop_size;
	vui_params->videoSignalTypePresentFlag = 1;
	vui_params->videoFullRangeFlag = (voi->range == VIDEO_RANGE_FULL);
	vui_params->colourDescriptionPresentFlag = 1;
	vui_params->colourMatrix = (voi->colorspace == VIDEO_CS_709) ? 1 : 5;
	vui_params->colourPrimaries = 1;
	vui_params->transferCharacteristics = 1;

	enc->bframes = bf > 0;

	/* lookahead */
	if (lookahead &&
	    nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_LOOKAHEAD, codec_guid)) {
		config->rcParams.lookaheadDepth = 8;
		config->rcParams.enableLookahead = 1;
	} else {
		lookahead = false;
	}

	/* psycho aq */
	if (nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ, codec_guid)) {
		config->rcParams.enableAQ = psycho_aq;
		config->rcParams.enableTemporalAQ = psycho_aq;
	}

	/* -------------------------- */
	/* rate control               */

	enc->can_change_bitrate =
		nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE,
			   codec_guid) &&
		!lookahead;

	config->rcParams.rateControlMode = twopass ? NV_ENC_PARAMS_RC_VBR_HQ
						   : NV_ENC_PARAMS_RC_VBR;

	if (astrcmpi(rc, "cqp") == 0 || astrcmpi(rc, "lossless") == 0) {
		if (astrcmpi(rc, "lossless") == 0)
			cqp = 0;

		config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
		config->rcParams.constQP.qpInterP = cqp;
		config->rcParams.constQP.qpInterB = cqp;
		config->rcParams.constQP.qpIntra = cqp;
		enc->can_change_bitrate = false;

		bitrate = 0;
		max_bitrate = 0;

	} else if (astrcmpi(rc, "vbr") != 0) { /* CBR by default */
		hevc_config->outputBufferingPeriodSEI = 1;
		config->rcParams.rateControlMode =
			twopass ? NV_ENC_PARAMS_RC_2_PASS_QUALITY
				: NV_ENC_PARAMS_RC_CBR;
	}

	hevc_config->outputPictureTimingSEI = 1;
	config->rcParams.averageBitRate = bitrate * 1000;
	config->rcParams.maxBitRate = vbr ? max_bitrate * 1000 : bitrate * 1000;

	/* -------------------------- */
	/* profile                    */

	if (astrcmpi(profile, "main10") == 0) {
		config->profileGUID = NV_ENC_HEVC_PROFILE_MAIN10_GUID;
	} else if (astrcmpi(profile, "rext") == 0) {
		config->profileGUID = NV_ENC_HEVC_PROFILE_FREXT_GUID;
	} else {
		config->profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
	}

	/* -------------------------- */
	/* initialize                 */

	if (NV_FAILED(nv.nvEncInitializeEncoder(enc->session, params))) {
		return false;
	}

	enc->buf_count = config->frameIntervalP +
			 config->rcParams.lookaheadDepth + EXTRA_BUFFERS;
	enc->output_delay = enc->buf_count - 1;

	info("jim-nvenc-hevc settings:\n"
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
	     "\tlookahead:    %s\n"
	     "\tpsycho_aq:    %s\n",
	     rc, bitrate, cqp, gop_size, preset, profile, enc->cx, enc->cy,
	     twopass ? "true" : "false", bf, lookahead ? "true" : "false",
	     psycho_aq ? "true" : "false");

	return true;
}

static bool init_bitstreams(struct nvenc_data *enc)
{
	da_reserve(enc->bitstreams, enc->buf_count);
	for (size_t i = 0; i < enc->buf_count; i++) {
		struct nv_bitstream bitstream;
		if (!nv_bitstream_init(enc, &bitstream)) {
			return false;
		}

		da_push_back(enc->bitstreams, &bitstream);
	}

	return true;
}

static bool init_textures(struct nvenc_data *enc)
{
	da_reserve(enc->bitstreams, enc->buf_count);
	for (size_t i = 0; i < enc->buf_count; i++) {
		struct nv_texture texture;
		if (!nv_texture_init(enc, &texture)) {
			return false;
		}

		da_push_back(enc->textures, &texture);
	}

	return true;
}

static void nvenc_destroy(void *data);

static void *nvenc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	NV_ENCODE_API_FUNCTION_LIST init = {NV_ENCODE_API_FUNCTION_LIST_VER};
	struct nvenc_data *enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;
	enc->first_packet = true;

	//PRISM/Wangshaohui/20201230/#3786/support HEVC
	enc->is_h265_encoder = false;

	/* this encoder requires shared textures, this cannot be used on a
	 * gpu other than the one OBS is currently running on. */
	int gpu = (int)obs_data_get_int(settings, "gpu");
	if (gpu != 0) {
		goto fail;
	}

	if (!obs_nv12_tex_active()) {
		goto fail;
	}
	if (!init_nvenc()) {
		goto fail;
	}
	if (NV_FAILED(nv_create_instance(&init))) {
		goto fail;
	}
	if (!init_d3d11(enc, settings)) {
		goto fail;
	}
	if (!init_session(enc)) {
		goto fail;
	}
	if (!init_encoder(enc, settings)) {
		goto fail;
	}
	if (!init_bitstreams(enc)) {
		goto fail;
	}
	if (!init_textures(enc)) {
		goto fail;
	}

	return enc;

fail:
	nvenc_destroy(enc);
	return obs_encoder_create_rerouted(encoder, "ffmpeg_nvenc");
}

static void *nvenc_hevc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	NV_ENCODE_API_FUNCTION_LIST init = {NV_ENCODE_API_FUNCTION_LIST_VER};
	struct nvenc_data *enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;
	enc->first_packet = true;
	enc->is_h265_encoder = true;

	/* this encoder requires shared textures, this cannot be used on a
	 * gpu other than the one OBS is currently running on. */
	int gpu = (int)obs_data_get_int(settings, "gpu");
	if (gpu != 0) {
		goto fail;
	}

	if (!obs_nv12_tex_active()) {
		goto fail;
	}
	if (!init_nvenc()) {
		goto fail;
	}
	if (NV_FAILED(nv_create_instance(&init))) {
		goto fail;
	}
	if (!init_d3d11(enc, settings)) {
		goto fail;
	}
	if (!init_session(enc)) {
		goto fail;
	}
	if (!init_encoder_hevc(enc, settings)) {
		goto fail;
	}
	if (!init_bitstreams(enc)) {
		goto fail;
	}
	if (!init_textures(enc)) {
		goto fail;
	}

	return enc;

fail:
	nvenc_destroy(enc);
	return obs_encoder_create_rerouted(encoder, "ffmpeg_nvenc_hevc");
}

static bool get_encoded_packet(struct nvenc_data *enc, bool finalize);

static void nvenc_destroy(void *data)
{
	struct nvenc_data *enc = data;

	if (enc->encode_started) {
		size_t next_bitstream = enc->next_bitstream;
		HANDLE next_event = enc->bitstreams.array[next_bitstream].event;

		NV_ENC_PIC_PARAMS params = {NV_ENC_PIC_PARAMS_VER};
		params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
		params.completionEvent = next_event;
		nv.nvEncEncodePicture(enc->session, &params);
		get_encoded_packet(enc, true);
	}
	for (size_t i = 0; i < enc->textures.num; i++) {
		nv_texture_free(enc, &enc->textures.array[i]);
	}
	for (size_t i = 0; i < enc->bitstreams.num; i++) {
		nv_bitstream_free(enc, &enc->bitstreams.array[i]);
	}
	if (enc->session) {
		nv.nvEncDestroyEncoder(enc->session);
	}
	for (size_t i = 0; i < enc->input_textures.num; i++) {
		ID3D11Texture2D *tex = enc->input_textures.array[i].tex;
		IDXGIKeyedMutex *km = enc->input_textures.array[i].km;
		tex->lpVtbl->Release(tex);
		km->lpVtbl->Release(km);
	}
	if (enc->context) {
		enc->context->lpVtbl->Release(enc->context);
	}
	if (enc->device) {
		enc->device->lpVtbl->Release(enc->device);
	}

	bfree(enc->header);
	bfree(enc->sei);
	circlebuf_free(&enc->dts_list);
	da_free(enc->textures);
	da_free(enc->bitstreams);
	da_free(enc->input_textures);
	da_free(enc->packet_data);
	bfree(enc);
}

static ID3D11Texture2D *get_tex_from_handle(struct nvenc_data *enc,
					    uint32_t handle,
					    IDXGIKeyedMutex **km_out)
{
	ID3D11Device *device = enc->device;
	IDXGIKeyedMutex *km;
	ID3D11Texture2D *input_tex;
	HRESULT hr;

	for (size_t i = 0; i < enc->input_textures.num; i++) {
		struct handle_tex *ht = &enc->input_textures.array[i];
		if (ht->handle == handle) {
			*km_out = ht->km;
			return ht->tex;
		}
	}

	hr = device->lpVtbl->OpenSharedResource(device,
						(HANDLE)(uintptr_t)handle,
						&IID_ID3D11Texture2D,
						&input_tex);
	if (FAILED(hr)) {
		error_hr("OpenSharedResource failed");
		return NULL;
	}

	hr = input_tex->lpVtbl->QueryInterface(input_tex, &IID_IDXGIKeyedMutex,
					       &km);
	if (FAILED(hr)) {
		error_hr("QueryInterface(IDXGIKeyedMutex) failed");
		input_tex->lpVtbl->Release(input_tex);
		return NULL;
	}

	input_tex->lpVtbl->SetEvictionPriority(input_tex,
					       DXGI_RESOURCE_PRIORITY_MAXIMUM);

	*km_out = km;

	struct handle_tex new_ht = {handle, input_tex, km};
	da_push_back(enc->input_textures, &new_ht);
	return input_tex;
}

static bool get_encoded_packet(struct nvenc_data *enc, bool finalize)
{
	void *s = enc->session;

	da_resize(enc->packet_data, 0);

	if (!enc->buffers_queued)
		return true;
	if (!finalize && enc->buffers_queued < enc->output_delay)
		return true;

	size_t count = finalize ? enc->buffers_queued : 1;

	for (size_t i = 0; i < count; i++) {
		size_t cur_bs_idx = enc->cur_bitstream;
		struct nv_bitstream *bs = &enc->bitstreams.array[cur_bs_idx];
		struct nv_texture *nvtex = &enc->textures.array[cur_bs_idx];

		/* ---------------- */

		NV_ENC_LOCK_BITSTREAM lock = {NV_ENC_LOCK_BITSTREAM_VER};
		lock.outputBitstream = bs->ptr;
		lock.doNotWait = false;

		if (NV_FAILED(nv.nvEncLockBitstream(s, &lock))) {
			return false;
		}

		if (enc->first_packet) {
			uint8_t *new_packet;
			size_t size;

			enc->first_packet = false;
			//PRISM/Wangshaohui/20201230/#3786/support HEVC
			if (enc->is_h265_encoder) {
				obs_extract_hevc_headers(
					lock.bitstreamBufferPtr,
					lock.bitstreamSizeInBytes, &new_packet,
					&size, &enc->header, &enc->header_size,
					&enc->sei, &enc->sei_size);
			} else {
				obs_extract_avc_headers(
					lock.bitstreamBufferPtr,
					lock.bitstreamSizeInBytes, &new_packet,
					&size, &enc->header, &enc->header_size,
					&enc->sei, &enc->sei_size);
			}

			da_copy_array(enc->packet_data, new_packet, size);
			bfree(new_packet);
		} else {
			da_copy_array(enc->packet_data, lock.bitstreamBufferPtr,
				      lock.bitstreamSizeInBytes);
		}

		enc->packet_pts = (int64_t)lock.outputTimeStamp;
		enc->packet_keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR;

		if (NV_FAILED(nv.nvEncUnlockBitstream(s, bs->ptr))) {
			return false;
		}

		/* ---------------- */

		if (nvtex->mapped_res) {
			NVENCSTATUS err;
			err = nv.nvEncUnmapInputResource(s, nvtex->mapped_res);
			if (nv_failed(enc, err, __FUNCTION__, "unmap")) {
				return false;
			}
			nvtex->mapped_res = NULL;
		}

		/* ---------------- */

		if (++enc->cur_bitstream == enc->buf_count)
			enc->cur_bitstream = 0;

		enc->buffers_queued--;
	}

	return true;
}

static bool nvenc_encode_tex(void *data, uint32_t handle, int64_t pts,
			     uint64_t lock_key, uint64_t *next_key,
			     struct encoder_packet *packet,
			     bool *received_packet)
{
	struct nvenc_data *enc = data;
	ID3D11Device *device = enc->device;
	ID3D11DeviceContext *context = enc->context;
	ID3D11Texture2D *input_tex;
	ID3D11Texture2D *output_tex;
	IDXGIKeyedMutex *km;
	struct nv_texture *nvtex;
	struct nv_bitstream *bs;
	NVENCSTATUS err;

	if (handle == GS_INVALID_HANDLE) {
		error("Encode failed: bad texture handle");
		*next_key = lock_key;
		return false;
	}

	bs = &enc->bitstreams.array[enc->next_bitstream];
	nvtex = &enc->textures.array[enc->next_bitstream];

	input_tex = get_tex_from_handle(enc, handle, &km);
	output_tex = nvtex->tex;

	if (!input_tex) {
		*next_key = lock_key;
		return false;
	}

	circlebuf_push_back(&enc->dts_list, &pts, sizeof(pts));

	/* ------------------------------------ */
	/* wait for output bitstream/tex        */

	WaitForSingleObject(bs->event, INFINITE);

	/* ------------------------------------ */
	/* copy to output tex                   */

	km->lpVtbl->AcquireSync(km, lock_key, INFINITE);

	context->lpVtbl->CopyResource(context, (ID3D11Resource *)output_tex,
				      (ID3D11Resource *)input_tex);

	km->lpVtbl->ReleaseSync(km, *next_key);

	/* ------------------------------------ */
	/* map output tex so nvenc can use it   */

	NV_ENC_MAP_INPUT_RESOURCE map = {NV_ENC_MAP_INPUT_RESOURCE_VER};
	map.registeredResource = nvtex->res;
	if (NV_FAILED(nv.nvEncMapInputResource(enc->session, &map))) {
		return false;
	}

	nvtex->mapped_res = map.mappedResource;

	/* ------------------------------------ */
	/* do actual encode call                */

	NV_ENC_PIC_PARAMS params = {0};
	params.version = NV_ENC_PIC_PARAMS_VER;
	params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	params.inputBuffer = nvtex->mapped_res;
	params.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
	params.inputTimeStamp = (uint64_t)pts;
	params.inputWidth = enc->cx;
	params.inputHeight = enc->cy;
	params.outputBitstream = bs->ptr;
	params.completionEvent = bs->event;

	err = nv.nvEncEncodePicture(enc->session, &params);
	if (err != NV_ENC_SUCCESS && err != NV_ENC_ERR_NEED_MORE_INPUT) {
		nv_failed(enc, err, __FUNCTION__, "nvEncEncodePicture");
		return false;
	}

	enc->encode_started = true;
	enc->buffers_queued++;

	if (++enc->next_bitstream == enc->buf_count) {
		enc->next_bitstream = 0;
	}

	/* ------------------------------------ */
	/* check for encoded packet and parse   */

	if (!get_encoded_packet(enc, false)) {
		return false;
	}

	/* ------------------------------------ */
	/* output encoded packet                */

	if (enc->packet_data.num) {
		int64_t dts;
		circlebuf_pop_front(&enc->dts_list, &dts, sizeof(dts));

		/* subtract bframe delay from dts */
		if (enc->bframes)
			dts -= packet->timebase_num;

		*received_packet = true;
		packet->data = enc->packet_data.array;
		packet->size = enc->packet_data.num;
		packet->type = OBS_ENCODER_VIDEO;
		packet->pts = enc->packet_pts;
		packet->dts = dts;
		packet->keyframe = enc->packet_keyframe;
	} else {
		*received_packet = false;
	}

	return true;
}

extern void nvenc_defaults(obs_data_t *settings);
extern obs_properties_t *nvenc_properties(void *unused);

//PRISM/Wangshaohui/20201230/#3786/support HEVC
extern void nvenc_hevc_defaults(obs_data_t *settings);
//PRISM/Wangshaohui/20201230/#3786/support HEVC
extern obs_properties_t *nvenc_properties_hevc(void *unused);

static bool nvenc_extra_data(void *data, uint8_t **header, size_t *size)
{
	struct nvenc_data *enc = data;

	if (!enc->header) {
		return false;
	}

	*header = enc->header;
	*size = enc->header_size;
	return true;
}

static bool nvenc_sei_data(void *data, uint8_t **sei, size_t *size)
{
	struct nvenc_data *enc = data;

	if (!enc->sei) {
		return false;
	}

	*sei = enc->sei;
	*size = enc->sei_size;
	return true;
}

//PRISM/ZengQin/20210528/#none/get encoder props params
static obs_data_t *get_props_params(void *data, bool hevc)
{
	if (!data)
		return NULL;

	struct nvenc_data *enc = data;
	obs_data_t *settings = obs_encoder_get_settings(enc->encoder);
	const char *rc = obs_data_get_string(settings, "rate_control");
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	int cqp = (int)obs_data_get_int(settings, "cqp");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	const char *preset = obs_data_get_string(settings, "preset");
	const char *profile = obs_data_get_string(settings, "profile");
	bool psycho_aq = obs_data_get_bool(settings, "psycho_aq");
	bool lookahead = obs_data_get_bool(settings, "lookahead");
	int bf = hevc ? 0 : (int)obs_data_get_int(settings, "bf");
	obs_data_release(settings);

	if (astrcmpi(rc, "cqp") == 0 || astrcmpi(rc, "lossless") == 0) {
		if (astrcmpi(rc, "lossless") == 0)
			cqp = 0;
		bitrate = 0;
	}

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	//PRISM/LiuHaibin/20210803/#9045/add protection division operation
	uint32_t gop_size = (keyint_sec && voi->fps_den)
				    ? keyint_sec * voi->fps_num / voi->fps_den
				    : 250;

	bool twopass = false;
	if (astrcmpi(preset, "mq") == 0) {
		twopass = true;
	}

	GUID codec_guid = hevc ? NV_ENC_CODEC_HEVC_GUID
			       : NV_ENC_CODEC_H264_GUID;
	if (!(lookahead &&
	      nv_get_cap(enc, NV_ENC_CAPS_SUPPORT_LOOKAHEAD, codec_guid))) {
		lookahead = false;
	}

	obs_data_t *params = obs_data_create();
	obs_data_set_string(params, "rate_control", rc);
	obs_data_set_int(params, "bitrate", bitrate);
	obs_data_set_int(params, "cqp", cqp);
	obs_data_set_int(params, "keyint", gop_size);
	obs_data_set_string(params, "preset", preset);
	obs_data_set_string(params, "profile", profile);
	obs_data_set_int(params, "width", enc->cx);
	obs_data_set_int(params, "height", enc->cy);
	obs_data_set_string(params, "2-pass", twopass ? "true" : "false");
	obs_data_set_int(params, "bframes", bf);
	obs_data_set_string(params, "lookahead", lookahead ? "true" : "false");
	obs_data_set_string(params, "psycho_aq", psycho_aq ? "true" : "false");
	return params;
}

//PRISM/ZengQin/20210528/#none/get encoder props params
static obs_data_t *nvenc_props_prams(void *data)
{
	return get_props_params(data, false);
}

//PRISM/ZengQin/20210528/#none/get encoder props params
static obs_data_t *nvenc_hevc_props_prams(void *data)
{
	return get_props_params(data, true);
}

struct obs_encoder_info nvenc_info = {
	.id = "jim_nvenc",
	.codec = "h264",
	.type = OBS_ENCODER_VIDEO,
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_DYN_BITRATE,
	.get_name = nvenc_get_name,
	.create = nvenc_create,
	.destroy = nvenc_destroy,
	.update = nvenc_update,
	.encode_texture = nvenc_encode_tex,
	.get_defaults = nvenc_defaults,
	.get_properties = nvenc_properties,
	.get_extra_data = nvenc_extra_data,
	.get_sei_data = nvenc_sei_data,
	//PRISM/ZengQin/20210528/#none/get encoder props params
	.props_params = nvenc_props_prams,
};

//PRISM/Wangshaohui/20201230/#3786/support HEVC
struct obs_encoder_info jim_nvenc_hevc_info = {
	.id = "jim_nvenc_hevc",
	.codec = "hevc",
	.type = OBS_ENCODER_VIDEO,
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_DYN_BITRATE,
	.get_name = nvenc_hevc_get_name,
	.create = nvenc_hevc_create,
	.destroy = nvenc_destroy,
	.update = nvenc_update,
	.encode_texture = nvenc_encode_tex,
	.get_defaults = nvenc_hevc_defaults,
	.get_properties = nvenc_properties_hevc,
	.get_extra_data = nvenc_extra_data,
	.get_sei_data = nvenc_sei_data,
	//PRISM/ZengQin/20210528/#none/get encoder props params
	.props_params = nvenc_hevc_props_prams,
};
