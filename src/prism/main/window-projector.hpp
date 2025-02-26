#pragma once

#include <obs.hpp>
#include "qt-display.hpp"

enum class ProjectorType {
	Source,
	Scene,
	Preview,
	StudioProgram,
	Multiview,
};

class QMouseEvent;

enum class MultiviewLayout : uint8_t {
	HORIZONTAL_TOP_8_SCENES = 0,
	HORIZONTAL_BOTTOM_8_SCENES = 1,
	VERTICAL_LEFT_8_SCENES = 2,
	VERTICAL_RIGHT_8_SCENES = 3,
	HORIZONTAL_TOP_24_SCENES = 4,
};

struct label_texture {
	gs_texture *render_texture;
	gs_texture *shader_input_texture;
	label_texture()
	{
		render_texture = nullptr;
		shader_input_texture = nullptr;
	}
};

class PLSProjector : public PLSQTDisplay {
	Q_OBJECT

private:
	OBSSource source;
	OBSSignal removedSignal;

	static void PLSRenderMultiview(void *data, uint32_t cx, uint32_t cy);
	static void PLSRender(void *data, uint32_t cx, uint32_t cy);
	static void OBSSourceRemoved(void *data, calldata_t *params);

	void mousePressEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	virtual void mouseMoveEvent(QMouseEvent *event) override;

	int savedMonitor;
	bool isWindow;
	QString projectorTitle;
	ProjectorType type = ProjectorType::Source;
	std::vector<OBSWeakSource> multiviewScenes;
	std::vector<OBSSource> multiviewLabels;
	std::vector<label_texture *> multiLabelTexture;
	gs_vertbuffer_t *actionSafeMargin = nullptr;
	gs_vertbuffer_t *graphicsSafeMargin = nullptr;
	gs_vertbuffer_t *fourByThreeSafeMargin = nullptr;
	gs_vertbuffer_t *leftLine = nullptr;
	gs_vertbuffer_t *topLine = nullptr;
	gs_vertbuffer_t *rightLine = nullptr;
	gs_effect_t *solid = nullptr;
	gs_eparam_t *color = nullptr;
	// Multiview position helpers
	float thickness = 4;
	float offset, thicknessx2 = thickness * 2, pvwprgCX, pvwprgCY, sourceX, sourceY, labelX, labelY, scenesCX, scenesCY, ppiCX, ppiCY, siX, siY, siCX, siCY, ppiScaleX, ppiScaleY, siScaleX,
		      siScaleY, fw, fh, ratio;

	float lineLength = 0.1f;
	// Rec. ITU-R BT.1848-1 / EBU R 95
	float actionSafePercentage = 0.035f;       // 3.5%
	float graphicsSafePercentage = 0.05f;      // 5.0%
	float fourByThreeSafePercentage = 0.1625f; // 16.25%
	bool ready = false;
	bool cursorHidden = false;

	// argb colors
	static const uint32_t outerColor = 0xFFD0D0D0;
	static const uint32_t labelColor = 0xD91F1F1F;
	static const uint32_t backgroundColor = 0xFF000000;
	static const uint32_t previewColor = 0xFF00D000;
	static const uint32_t programColor = 0xFFD00000;

	void UpdateMultiview();
	void UpdateProjectorTitle(QString name);

private slots:
	void EscapeTriggered();

public:
	explicit PLSProjector(QWidget *widget, obs_source_t *source_, int monitor, QString title, ProjectorType type_);
	~PLSProjector();

	OBSSource GetSource();
	ProjectorType GetProjectorType();
	int GetMonitor();
	static void UpdateMultiviewProjectors();
	static void RenameProjector(QString oldName, QString newName);
};
