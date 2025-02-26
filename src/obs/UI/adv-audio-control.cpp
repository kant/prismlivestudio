#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include "qt-wrappers.hpp"
#include "obs-app.hpp"
#include "adv-audio-control.hpp"
#include "window-basic-main.hpp"

#ifndef NSEC_PER_MSEC
#define NSEC_PER_MSEC 1000000
#endif

#define MIN_DB -96.0
#define MAX_DB 26.0

OBSAdvAudioCtrl::OBSAdvAudioCtrl(QGridLayout *, obs_source_t *source_)
	: source(source_)
{
	QHBoxLayout *hlayout;
	signal_handler_t *handler = obs_source_get_signal_handler(source);
	const char *sourceName = obs_source_get_name(source);
	float vol = obs_source_get_volume(source);
	uint32_t flags = obs_source_get_flags(source);
	uint32_t mixers = obs_source_get_audio_mixers(source);

	forceMonoContainer = new QWidget();
	mixerContainer = new QWidget();
	balanceContainer = new QWidget();
	labelL = new QLabel();
	labelR = new QLabel();
	nameLabel = new QLabel();
	volume = new QDoubleSpinBox();
	forceMono = new QCheckBox();
	balance = new BalanceSlider();
#if defined(_WIN32) || defined(__APPLE__) || HAVE_PULSEAUDIO
	monitoringType = new QComboBox();
#endif
	syncOffset = new QSpinBox();
	mixer1 = new QCheckBox();
	mixer2 = new QCheckBox();
	mixer3 = new QCheckBox();
	mixer4 = new QCheckBox();
	mixer5 = new QCheckBox();
	mixer6 = new QCheckBox();

	volChangedSignal.Connect(handler, "volume", OBSSourceVolumeChanged,
				 this);
	syncOffsetSignal.Connect(handler, "audio_sync", OBSSourceSyncChanged,
				 this);
	flagsSignal.Connect(handler, "update_flags", OBSSourceFlagsChanged,
			    this);
	mixersSignal.Connect(handler, "audio_mixers", OBSSourceMixersChanged,
			     this);

	hlayout = new QHBoxLayout();
	hlayout->setContentsMargins(0, 0, 0, 0);
	forceMonoContainer->setLayout(hlayout);
	hlayout = new QHBoxLayout();
	hlayout->setContentsMargins(0, 0, 0, 0);
	mixerContainer->setLayout(hlayout);
	hlayout = new QHBoxLayout();
	hlayout->setContentsMargins(0, 0, 0, 0);
	balanceContainer->setLayout(hlayout);
	balanceContainer->setFixedWidth(150);

	labelL->setText("L");

	labelR->setText("R");

	nameLabel->setText(QT_UTF8(sourceName));
	nameLabel->setAlignment(Qt::AlignVCenter);

	volume->setMinimum(MIN_DB - 0.1);
	volume->setMaximum(MAX_DB);
	volume->setSingleStep(0.1);
	volume->setDecimals(1);
	volume->setSuffix(" dB");
	volume->setValue(obs_mul_to_db(vol));
	volume->setFixedWidth(100);

	if (volume->value() < MIN_DB)
		volume->setSpecialValueText("-inf dB");

	forceMono->setChecked((flags & OBS_SOURCE_FLAG_FORCE_MONO) != 0);

	forceMonoContainer->layout()->addWidget(forceMono);
	forceMonoContainer->layout()->setAlignment(forceMono, Qt::AlignVCenter);
	forceMonoContainer->setFixedWidth(50);

	balance->setOrientation(Qt::Horizontal);
	balance->setMinimum(0);
	balance->setMaximum(100);
	balance->setTickPosition(QSlider::TicksAbove);
	balance->setTickInterval(50);

	OBSBasic *main = reinterpret_cast<OBSBasic *>(App()->GetMainWindow());

	const char *speakers =
		config_get_string(main->Config(), "Audio", "ChannelSetup");

	if (strcmp(speakers, "Mono") == 0)
		balance->setEnabled(false);
	else
		balance->setEnabled(true);

	float bal = obs_source_get_balance_value(source) * 100.0f;
	balance->setValue((int)bal);

	int64_t cur_sync = obs_source_get_sync_offset(source);
	syncOffset->setMinimum(-950);
	syncOffset->setMaximum(20000);
	syncOffset->setSuffix(" ms");
	syncOffset->setValue(int(cur_sync / NSEC_PER_MSEC));
	syncOffset->setFixedWidth(100);

	int idx;
#if defined(_WIN32) || defined(__APPLE__) || HAVE_PULSEAUDIO
	monitoringType->addItem(QTStr("Basic.AdvAudio.Monitoring.None"),
				(int)OBS_MONITORING_TYPE_NONE);
	monitoringType->addItem(QTStr("Basic.AdvAudio.Monitoring.MonitorOnly"),
				(int)OBS_MONITORING_TYPE_MONITOR_ONLY);
	monitoringType->addItem(QTStr("Basic.AdvAudio.Monitoring.Both"),
				(int)OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
	int mt = (int)obs_source_get_monitoring_type(source);
	idx = monitoringType->findData(mt);
	monitoringType->setCurrentIndex(idx);
#endif

	mixer1->setText("1");
	mixer1->setChecked(mixers & (1 << 0));
	mixer2->setText("2");
	mixer2->setChecked(mixers & (1 << 1));
	mixer3->setText("3");
	mixer3->setChecked(mixers & (1 << 2));
	mixer4->setText("4");
	mixer4->setChecked(mixers & (1 << 3));
	mixer5->setText("5");
	mixer5->setChecked(mixers & (1 << 4));
	mixer6->setText("6");
	mixer6->setChecked(mixers & (1 << 5));

	speaker_layout sl = obs_source_get_speaker_layout(source);

	if (sl == SPEAKERS_STEREO) {
		balanceContainer->layout()->addWidget(labelL);
		balanceContainer->layout()->addWidget(balance);
		balanceContainer->layout()->addWidget(labelR);
		balanceContainer->setMaximumWidth(170);
	}

	mixerContainer->layout()->addWidget(mixer1);
	mixerContainer->layout()->addWidget(mixer2);
	mixerContainer->layout()->addWidget(mixer3);
	mixerContainer->layout()->addWidget(mixer4);
	mixerContainer->layout()->addWidget(mixer5);
	mixerContainer->layout()->addWidget(mixer6);

	QWidget::connect(volume, SIGNAL(valueChanged(double)), this,
			 SLOT(volumeChanged(double)));
	QWidget::connect(forceMono, SIGNAL(clicked(bool)), this,
			 SLOT(downmixMonoChanged(bool)));
	QWidget::connect(balance, SIGNAL(valueChanged(int)), this,
			 SLOT(balanceChanged(int)));
	QWidget::connect(balance, SIGNAL(doubleClicked()), this,
			 SLOT(ResetBalance()));
	QWidget::connect(syncOffset, SIGNAL(valueChanged(int)), this,
			 SLOT(syncOffsetChanged(int)));
#if defined(_WIN32) || defined(__APPLE__) || HAVE_PULSEAUDIO
	QWidget::connect(monitoringType, SIGNAL(currentIndexChanged(int)), this,
			 SLOT(monitoringTypeChanged(int)));
#endif
	QWidget::connect(mixer1, SIGNAL(clicked(bool)), this,
			 SLOT(mixer1Changed(bool)));
	QWidget::connect(mixer2, SIGNAL(clicked(bool)), this,
			 SLOT(mixer2Changed(bool)));
	QWidget::connect(mixer3, SIGNAL(clicked(bool)), this,
			 SLOT(mixer3Changed(bool)));
	QWidget::connect(mixer4, SIGNAL(clicked(bool)), this,
			 SLOT(mixer4Changed(bool)));
	QWidget::connect(mixer5, SIGNAL(clicked(bool)), this,
			 SLOT(mixer5Changed(bool)));
	QWidget::connect(mixer6, SIGNAL(clicked(bool)), this,
			 SLOT(mixer6Changed(bool)));

	setObjectName(sourceName);
}

OBSAdvAudioCtrl::~OBSAdvAudioCtrl()
{
	nameLabel->deleteLater();
	volume->deleteLater();
	forceMonoContainer->deleteLater();
	balanceContainer->deleteLater();
	syncOffset->deleteLater();
#if defined(_WIN32) || defined(__APPLE__) || HAVE_PULSEAUDIO
	monitoringType->deleteLater();
#endif
	mixerContainer->deleteLater();
}

void OBSAdvAudioCtrl::ShowAudioControl(QGridLayout *layout)
{
	int lastRow = layout->rowCount();
	int idx = 0;

	layout->addWidget(nameLabel, lastRow, idx++);
	layout->addWidget(volume, lastRow, idx++);
	layout->addWidget(forceMonoContainer, lastRow, idx++);
	layout->addWidget(balanceContainer, lastRow, idx++);
	layout->addWidget(syncOffset, lastRow, idx++);
#if defined(_WIN32) || defined(__APPLE__) || HAVE_PULSEAUDIO
	layout->addWidget(monitoringType, lastRow, idx++);
#endif
	layout->addWidget(mixerContainer, lastRow, idx++);
	layout->layout()->setAlignment(mixerContainer, Qt::AlignVCenter);
	layout->setHorizontalSpacing(15);
}

/* ------------------------------------------------------------------------- */
/* OBS source callbacks */

void OBSAdvAudioCtrl::OBSSourceFlagsChanged(void *param, calldata_t *calldata)
{
	uint32_t flags = (uint32_t)calldata_int(calldata, "flags");
	QMetaObject::invokeMethod(reinterpret_cast<OBSAdvAudioCtrl *>(param),
				  "SourceFlagsChanged", Q_ARG(uint32_t, flags));
}

void OBSAdvAudioCtrl::OBSSourceVolumeChanged(void *param, calldata_t *calldata)
{
	float volume = (float)calldata_float(calldata, "volume");
	QMetaObject::invokeMethod(reinterpret_cast<OBSAdvAudioCtrl *>(param),
				  "SourceVolumeChanged", Q_ARG(float, volume));
}

void OBSAdvAudioCtrl::OBSSourceSyncChanged(void *param, calldata_t *calldata)
{
	int64_t offset = calldata_int(calldata, "offset");
	QMetaObject::invokeMethod(reinterpret_cast<OBSAdvAudioCtrl *>(param),
				  "SourceSyncChanged", Q_ARG(int64_t, offset));
}

void OBSAdvAudioCtrl::OBSSourceMixersChanged(void *param, calldata_t *calldata)
{
	uint32_t mixers = (uint32_t)calldata_int(calldata, "mixers");
	QMetaObject::invokeMethod(reinterpret_cast<OBSAdvAudioCtrl *>(param),
				  "SourceMixersChanged",
				  Q_ARG(uint32_t, mixers));
}

/* ------------------------------------------------------------------------- */
/* Qt event queue source callbacks */

static inline void setCheckboxState(QCheckBox *checkbox, bool checked)
{
	checkbox->blockSignals(true);
	checkbox->setChecked(checked);
	checkbox->blockSignals(false);
}

void OBSAdvAudioCtrl::SourceFlagsChanged(uint32_t flags)
{
	bool forceMonoVal = (flags & OBS_SOURCE_FLAG_FORCE_MONO) != 0;
	setCheckboxState(forceMono, forceMonoVal);
}

void OBSAdvAudioCtrl::SourceVolumeChanged(float value)
{
	volume->blockSignals(true);
	volume->setValue(obs_mul_to_db(value));
	volume->blockSignals(false);
}

void OBSAdvAudioCtrl::SourceSyncChanged(int64_t offset)
{
	syncOffset->setValue(offset / NSEC_PER_MSEC);
}

void OBSAdvAudioCtrl::SourceMixersChanged(uint32_t mixers)
{
	setCheckboxState(mixer1, mixers & (1 << 0));
	setCheckboxState(mixer2, mixers & (1 << 1));
	setCheckboxState(mixer3, mixers & (1 << 2));
	setCheckboxState(mixer4, mixers & (1 << 3));
	setCheckboxState(mixer5, mixers & (1 << 4));
	setCheckboxState(mixer6, mixers & (1 << 5));
}

/* ------------------------------------------------------------------------- */
/* Qt control callbacks */

void OBSAdvAudioCtrl::volumeChanged(double db)
{
	if (db < MIN_DB) {
		volume->setSpecialValueText("-inf dB");
		db = -INFINITY;
	}

	float val = obs_db_to_mul(db);
	obs_source_set_volume(source, val);
}

void OBSAdvAudioCtrl::downmixMonoChanged(bool checked)
{
	uint32_t flags = obs_source_get_flags(source);
	bool forceMonoActive = (flags & OBS_SOURCE_FLAG_FORCE_MONO) != 0;

	if (forceMonoActive != checked) {
		if (checked)
			flags |= OBS_SOURCE_FLAG_FORCE_MONO;
		else
			flags &= ~OBS_SOURCE_FLAG_FORCE_MONO;

		obs_source_set_flags(source, flags);
	}
}

void OBSAdvAudioCtrl::balanceChanged(int val)
{
	float bal = (float)val / 100.0f;

	if (abs(50 - val) < 10) {
		balance->blockSignals(true);
		balance->setValue(50);
		bal = 0.5f;
		balance->blockSignals(false);
	}

	obs_source_set_balance_value(source, bal);
}

void OBSAdvAudioCtrl::ResetBalance()
{
	balance->setValue(50);
}

void OBSAdvAudioCtrl::syncOffsetChanged(int milliseconds)
{
	int64_t cur_val = obs_source_get_sync_offset(source);

	if (cur_val / NSEC_PER_MSEC != milliseconds)
		obs_source_set_sync_offset(source, int64_t(milliseconds) *
							   NSEC_PER_MSEC);
}

void OBSAdvAudioCtrl::monitoringTypeChanged(int index)
{
	int mt = monitoringType->itemData(index).toInt();
	obs_source_set_monitoring_type(source, (obs_monitoring_type)mt);

	const char *type = nullptr;

	switch (mt) {
	case OBS_MONITORING_TYPE_NONE:
		type = "none";
		break;
	case OBS_MONITORING_TYPE_MONITOR_ONLY:
		type = "monitor only";
		break;
	case OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT:
		type = "monitor and output";
		break;
	}

	plog(LOG_INFO, "User changed audio monitoring for source '%s' to: %s",
	     obs_source_get_name(source), type);
}

static inline void setMixer(obs_source_t *source, const int mixerIdx,
			    const bool checked)
{
	uint32_t mixers = obs_source_get_audio_mixers(source);
	uint32_t new_mixers = mixers;

	if (checked)
		new_mixers |= (1 << mixerIdx);
	else
		new_mixers &= ~(1 << mixerIdx);

	obs_source_set_audio_mixers(source, new_mixers);
}

void OBSAdvAudioCtrl::mixer1Changed(bool checked)
{
	setMixer(source, 0, checked);
}

void OBSAdvAudioCtrl::mixer2Changed(bool checked)
{
	setMixer(source, 1, checked);
}

void OBSAdvAudioCtrl::mixer3Changed(bool checked)
{
	setMixer(source, 2, checked);
}

void OBSAdvAudioCtrl::mixer4Changed(bool checked)
{
	setMixer(source, 3, checked);
}

void OBSAdvAudioCtrl::mixer5Changed(bool checked)
{
	setMixer(source, 4, checked);
}

void OBSAdvAudioCtrl::mixer6Changed(bool checked)
{
	setMixer(source, 5, checked);
}
