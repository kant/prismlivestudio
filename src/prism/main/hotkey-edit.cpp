/******************************************************************************
    Copyright (C) 2014-2015 by Ruwen Hahn <palana@stunned.de>

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

#include "hotkey-edit.hpp"

#include <util/dstr.hpp>
#include <QPointer>
#include <QStyle>

#include "pls-app.hpp"
#include "qt-wrappers.hpp"

static inline bool operator!=(const obs_key_combination_t &c1, const obs_key_combination_t &c2)
{
	return c1.modifiers != c2.modifiers || c1.key != c2.key;
}

static inline bool operator==(const obs_key_combination_t &c1, const obs_key_combination_t &c2)
{
	return !(c1 != c2);
}

void PLSHotkeyEdit::keyPressEvent(QKeyEvent *event)
{
	if (event->isAutoRepeat())
		return;

	obs_key_combination_t new_key;

	switch (event->key()) {
	case Qt::Key_Shift:
	case Qt::Key_Control:
	case Qt::Key_Alt:
	case Qt::Key_Meta:
		new_key.key = OBS_KEY_NONE;
		break;

#ifdef __APPLE__
	case Qt::Key_CapsLock:
		// kVK_CapsLock == 57
		new_key.key = obs_key_from_virtual_key(57);
		break;
#endif

	default:
		new_key.key = obs_key_from_virtual_key(event->nativeVirtualKey());
	}

	new_key.modifiers = TranslateQtKeyboardEventModifiers(event->modifiers());

	HandleNewKey(new_key);
}

#ifdef __APPLE__
void PLSHotkeyEdit::keyReleaseEvent(QKeyEvent *event)
{
	if (event->isAutoRepeat())
		return;

	if (event->key() != Qt::Key_CapsLock)
		return;

	obs_key_combination_t new_key;

	// kVK_CapsLock == 57
	new_key.key = obs_key_from_virtual_key(57);
	new_key.modifiers = TranslateQtKeyboardEventModifiers(event->modifiers());

	HandleNewKey(new_key);
}
#endif

void PLSHotkeyEdit::mousePressEvent(QMouseEvent *event)
{
	obs_key_combination_t new_key;

	switch (event->button()) {
	case Qt::NoButton:
	case Qt::LeftButton:
	case Qt::RightButton:
	case Qt::AllButtons:
	case Qt::MouseButtonMask:
		return;

	case Qt::MidButton:
		new_key.key = OBS_KEY_MOUSE3;
		break;

#define MAP_BUTTON(i, j)                        \
	case Qt::ExtraButton##i:                \
		new_key.key = OBS_KEY_MOUSE##j; \
		break;
		MAP_BUTTON(1, 4)
		MAP_BUTTON(2, 5)
		MAP_BUTTON(3, 6)
		MAP_BUTTON(4, 7)
		MAP_BUTTON(5, 8)
		MAP_BUTTON(6, 9)
		MAP_BUTTON(7, 10)
		MAP_BUTTON(8, 11)
		MAP_BUTTON(9, 12)
		MAP_BUTTON(10, 13)
		MAP_BUTTON(11, 14)
		MAP_BUTTON(12, 15)
		MAP_BUTTON(13, 16)
		MAP_BUTTON(14, 17)
		MAP_BUTTON(15, 18)
		MAP_BUTTON(16, 19)
		MAP_BUTTON(17, 20)
		MAP_BUTTON(18, 21)
		MAP_BUTTON(19, 22)
		MAP_BUTTON(20, 23)
		MAP_BUTTON(21, 24)
		MAP_BUTTON(22, 25)
		MAP_BUTTON(23, 26)
		MAP_BUTTON(24, 27)
#undef MAP_BUTTON
	}

	new_key.modifiers = TranslateQtKeyboardEventModifiers(event->modifiers());

	HandleNewKey(new_key);
}

void PLSHotkeyEdit::HandleNewKey(obs_key_combination_t new_key)
{
	if (new_key == key || obs_key_combination_is_empty(new_key))
		return;

	key = new_key;

	changed = true;
	emit KeyChanged(key);

	RenderKey();
}

void PLSHotkeyEdit::RenderKey()
{
	DStr str;
	obs_key_combination_to_str(key, str);

	setText(QT_UTF8(str));
}

void PLSHotkeyEdit::ResetKey()
{
	key = original;

	changed = false;
	emit KeyChanged(key);

	RenderKey();
}

void PLSHotkeyEdit::ClearKey()
{
	key = {0, OBS_KEY_NONE};

	changed = true;
	emit KeyChanged(key);

	RenderKey();
}

void PLSHotkeyEdit::InitSignalHandler()
{
	layoutChanged = {obs_get_signal_handler(), "hotkey_layout_change",
			 [](void *this_, calldata_t *) {
				 auto edit = static_cast<PLSHotkeyEdit *>(this_);
				 QMetaObject::invokeMethod(edit, "ReloadKeyLayout");
			 },
			 this};
}

void PLSHotkeyEdit::ReloadKeyLayout()
{
	RenderKey();
}

void PLSHotkeyWidget::SetKeyCombinations(const std::vector<obs_key_combination_t> &combos)
{
	if (combos.empty())
		AddEdit({0, OBS_KEY_NONE});

	for (auto combo : combos)
		AddEdit(combo);
}

bool PLSHotkeyWidget::Changed() const
{
	return changed || std::any_of(begin(edits), end(edits), [](PLSHotkeyEdit *edit) { return edit->changed; });
}

void PLSHotkeyWidget::Apply()
{
	for (auto &edit : edits) {
		edit->original = edit->key;
		edit->changed = false;
	}

	changed = false;
}

void PLSHotkeyWidget::GetCombinations(std::vector<obs_key_combination_t> &combinations) const
{
	combinations.clear();
	for (auto &edit : edits)
		if (!obs_key_combination_is_empty(edit->key))
			combinations.emplace_back(edit->key);
}

void PLSHotkeyWidget::Save()
{
	std::vector<obs_key_combination_t> combinations;
	Save(combinations);
}

void PLSHotkeyWidget::Save(std::vector<obs_key_combination_t> &combinations)
{
	GetCombinations(combinations);
	Apply();

	auto AtomicUpdate = [&]() {
		ignoreChangedBindings = true;

		obs_hotkey_load_bindings(id, combinations.data(), combinations.size());

		ignoreChangedBindings = false;
	};
	using AtomicUpdate_t = decltype(&AtomicUpdate);

	obs_hotkey_update_atomic([](void *d) { (*static_cast<AtomicUpdate_t>(d))(); }, static_cast<void *>(&AtomicUpdate));
}

void PLSHotkeyWidget::Clear()
{
	for (auto &edit : edits) {
		edit->ClearKey();
	}
}

void PLSHotkeyWidget::AddEdit(obs_key_combination combo, int idx)
{
	auto edit = new PLSHotkeyEdit(combo);
	edit->setToolTip(toolTip);

	clearBtn = new QPushButton;
	clearBtn->setToolTip(QTStr("Clear"));
	clearBtn->setFixedSize(24, 24);
	clearBtn->setFlat(true);
	clearBtn->setEnabled(clearBtnEnabled && !obs_key_combination_is_empty(combo));
	clearBtn->setProperty("uistep", QString::fromStdString(name));
	pls_flush_style(clearBtn);

	QObject::connect(edit, &PLSHotkeyEdit::KeyChanged, [=](obs_key_combination_t new_combo) {
		clearBtn->setEnabled(clearBtnEnabled && !obs_key_combination_is_empty(new_combo));
		pls_flush_style(clearBtn);
	});

	QHBoxLayout *subLayout = new QHBoxLayout;
	subLayout->setContentsMargins(0, 0, 0, 0);
	subLayout->addWidget(edit);
	subLayout->addWidget(clearBtn);

	if (idx != -1) {
		edits.insert(begin(edits) + idx, edit);
	} else {
		edits.emplace_back(edit);
	}

	layout()->insertLayout(idx, subLayout);

	QObject::connect(clearBtn, &QPushButton::clicked, this, [edit, this]() {
		clearButtonClicked(clearBtn);
		edit->ClearKey();
	});

	QObject::connect(edit, &PLSHotkeyEdit::KeyChanged, [&](obs_key_combination) { emit KeyChanged(); });
}

void PLSHotkeyWidget::RemoveEdit(size_t idx, bool signal)
{
	auto &edit = *(begin(edits) + idx);
	if (!obs_key_combination_is_empty(edit->original) && signal) {
		changed = true;
		emit KeyChanged();
	}

	edits.erase(begin(edits) + idx);

	auto item = layout()->takeAt(static_cast<int>(idx));
	QLayoutItem *child = nullptr;
	while ((child = item->layout()->takeAt(0))) {
		delete child->widget();
		delete child;
	}
	delete item;
}

void PLSHotkeyWidget::BindingsChanged(void *data, calldata_t *param)
{
	auto widget = static_cast<PLSHotkeyWidget *>(data);
	auto key = static_cast<obs_hotkey_t *>(calldata_ptr(param, "key"));

	QMetaObject::invokeMethod(widget, "HandleChangedBindings", Q_ARG(obs_hotkey_id, obs_hotkey_get_id(key)));
}

void PLSHotkeyWidget::HandleChangedBindings(obs_hotkey_id id_)
{
	if (ignoreChangedBindings || id != id_)
		return;

	std::vector<obs_key_combination_t> bindings;
	auto LoadBindings = [&](obs_hotkey_binding_t *binding) {
		if (obs_hotkey_binding_get_hotkey_id(binding) != id)
			return;

		auto get_combo = obs_hotkey_binding_get_key_combination;
		bindings.push_back(get_combo(binding));
	};
	using LoadBindings_t = decltype(&LoadBindings);

	obs_enum_hotkey_bindings(
		[](void *data, size_t, obs_hotkey_binding_t *binding) {
			auto LoadBindings = *static_cast<LoadBindings_t>(data);
			LoadBindings(binding);
			return true;
		},
		static_cast<void *>(&LoadBindings));

	while (edits.size() > 0)
		RemoveEdit(edits.size() - 1, false);

	SetKeyCombinations(bindings);
}

static inline void updateStyle(QWidget *widget)
{
	auto style = widget->style();
	style->unpolish(widget);
	style->polish(widget);
	widget->update();
}

void PLSHotkeyWidget::enterEvent(QEvent *event)
{
	if (!label)
		return;

	event->accept();
	label->highlightPair(true);
}

void PLSHotkeyWidget::leaveEvent(QEvent *event)
{
	if (!label)
		return;

	event->accept();
	label->highlightPair(false);
}

QString PLSHotkeyWidget::getHotkeyText() const
{
	QString hotkeyText;
	for (auto &edit : edits) {
		hotkeyText.append(edit->text());
	}
	return hotkeyText;
}

void PLSHotkeyWidget::setClearBtnDisabled()
{
	clearBtnEnabled = false;

	if (clearBtn) {
		clearBtn->setEnabled(clearBtnEnabled && clearBtn->isEnabled());
		pls_flush_style(clearBtn);
	}
}

void PLSHotkeyLabel::highlightPair(bool highlight)
{
	if (!pairPartner)
		return;

	pairPartner->setProperty("hotkeyPairHover", highlight);
	updateStyle(pairPartner);
	setProperty("hotkeyPairHover", highlight);
	updateStyle(this);
}

void PLSHotkeyLabel::enterEvent(QEvent *event)
{
	if (!pairPartner)
		return;

	event->accept();
	highlightPair(true);
}

void PLSHotkeyLabel::leaveEvent(QEvent *event)
{
	if (!pairPartner)
		return;

	event->accept();
	highlightPair(false);
}

void PLSHotkeyLabel::setToolTip(const QString &toolTip)
{
	QLabel::setToolTip(toolTip);
	if (widget)
		widget->setToolTip(toolTip);
}
