/******************************************************************************
 Copyright (C) 2014 by John R. Bradley <jrb@turrettech.com>
 Copyright (C) 2018 by Hugh Bailey ("Jim") <jim@obsproject.com>

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

#pragma once

#include <obs-module.h>
#include <obs.hpp>

#include "cef-headers.hpp"
#include "browser-config.h"
#include "browser-app.hpp"

//PRISM/Wangshaohui/20200811/#3784/for cef interaction
#include "interaction/interaction_main.h"

#include <unordered_map>
#include <functional>
#include <vector>
#include <string>
#include <mutex>

#if EXPERIMENTAL_SHARED_TEXTURE_SUPPORT_ENABLED
extern bool hwaccel;
#endif

//PRISM/Wangshaohui/20200811/#3784/for cef interaction
typedef std::function<void(INTERACTION_PTR)> InteractionFunc;

struct AudioStream {
	OBSSource source;
	speaker_layout speakers;
	int channels;
	int sample_rate;
};

struct BrowserSource {
	BrowserSource **p_prev_next = nullptr;
	BrowserSource *next = nullptr;

	obs_source_t *source = nullptr;

	bool tex_sharing_avail = false;
	bool create_browser = false;
	CefRefPtr<CefBrowser> cefBrowser;
	//PRISM/Wangshaohui/20211021/#10074/for threadsafe
	std::recursive_mutex lockBrowser;

	std::string url;
	std::string css;
	gs_texture_t *texture = nullptr;
	int width = 0;
	int height = 0;
	bool fps_custom = false;
	int fps = 0;
	bool restart = false;
	bool shutdown_on_invisible = false;
	bool is_local = false;
	bool first_update = true;
	bool reroute_audio = true;
#if EXPERIMENTAL_SHARED_TEXTURE_SUPPORT_ENABLED
	bool reset_frame = false;
#endif
	bool is_showing = false;

	//PRISM/Wangshaohui/20201021/#5271/for cef hardware accelerate
	bool use_hardware = true;

	inline void DestroyTextures()
	{
		if (texture) {
			obs_enter_graphics();
			gs_texture_destroy(texture);
			texture = nullptr;
			obs_leave_graphics();
		}
	}

	/* ------------------------------PRISM/Wangshaohui/20200811/#3784/for cef interaction --------------begin */
	INTERACTION_PTR interaction_ui;
	volatile bool is_interaction_showing = false;
	volatile bool is_interaction_reshow = false;

	// reference_source is for display, to make sure source won't be released while display is added
	// reference_source and interaction_display are created/deleted in video render thread
	obs_source_t *reference_source = NULL;
	obs_display_t *interaction_display = NULL;
	int display_cx = 0;
	int display_cy = 0;

	void ShowInteraction(bool show);
	void DestroyInteraction();
	void ExecuteOnInteraction(InteractionFunc func, bool async = false);
	void PostInteractionTitle();

	// Invoked from video render thread
	void OnInteractionShow(HWND hWnd);
	void OnInteractionHide(HWND hWnd);
	bool CreateDisplay(HWND hWnd, int cx, int cy);
	void ClearDisplay();

	static void SourceRenamed(void *data, calldata_t *pr);
	static void DrawPreview(void *data, uint32_t cx, uint32_t cy);
	static bool SetBrowserData(void *data, obs_data_t *private_data);
	static void GetBrowserData(void *data, obs_data_t *data_output);
	/* ------------------------------PRISM/Wangshaohui/20200811/#3784/for cef interaction --------------end */

	//PRISM/Wangshaohui/20211021/#10074/for threadsafe
	void SetBrowser(CefRefPtr<CefBrowser> b);
	CefRefPtr<CefBrowser> GetBrowser();

	bool CreateBrowser();
	void DestroyBrowser(bool async = false);
	void ClearAudioStreams();
	void ExecuteOnBrowser(BrowserFunc func, bool async = false);

	/* ---------------------------- */

	BrowserSource(obs_data_t *settings, obs_source_t *source);
	~BrowserSource();

	void Update(obs_data_t *settings = nullptr);
	void Tick();
	void Render();
	void EnumAudioStreams(obs_source_enum_proc_t cb, void *param);
	bool AudioMix(uint64_t *ts_out, struct audio_output_data *audio_output,
		      size_t channels, size_t sample_rate);

	//PRISM/Wangshaohui/20200811/#3784/for cef interaction
	/*
	void SendMouseClick(const struct obs_mouse_event *event, int32_t type,
			    bool mouse_up, uint32_t click_count);
	void SendMouseMove(const struct obs_mouse_event *event,
			   bool mouse_leave);
	void SendMouseWheel(const struct obs_mouse_event *event, int x_delta,
			    int y_delta);
	void SendFocus(bool focus);
	void SendKeyClick(const struct obs_key_event *event, bool key_up);
	*/

	void SetShowing(bool showing);
	void SetActive(bool active);
	void Refresh();

	void receiveWebMessage(const char *msg);

#if EXPERIMENTAL_SHARED_TEXTURE_SUPPORT_ENABLED
	inline void SignalBeginFrame();
#endif
	//PRISM/Zhangdewen/20200901/#for chat source initialization and send events to the web page
	virtual void onBrowserLoadEnd();

	std::mutex audio_sources_mutex;
	std::vector<obs_source_t *> audio_sources;

	std::unordered_map<int, AudioStream> audio_streams;
};
