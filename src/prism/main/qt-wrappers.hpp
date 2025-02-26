/******************************************************************************
    Copyright (C) 2013 by Hugh Bailey <obs.jim@gmail.com>

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

#include <QApplication>
#include <QWidget>
#include <QThread>
#include <obs.hpp>

#include <functional>
#include <memory>
#include <vector>

#include "alert-view.hpp"

#define QT_UTF8(str) QString::fromUtf8(str)
#define QT_TO_UTF8(str) str.toUtf8().constData()

class QDataStream;
class QWidget;
class QLayout;
class QString;
struct gs_window;

class PLSMessageBox {
public:
	static PLSAlertView::Button question(QWidget *parent, const QString &title, const QString &text,
					     PLSAlertView::Buttons buttons = PLSAlertView::Buttons(PLSAlertView::Button::Yes | PLSAlertView::Button::No),
					     PLSAlertView::Button defaultButton = PLSAlertView::Button::NoButton);
	static PLSAlertView::Button question(QWidget *parent, const QString &title, const QString &textTitle, const QString &textContent,
					     PLSAlertView::Buttons buttons = PLSAlertView::Buttons(PLSAlertView::Button::Yes | PLSAlertView::Button::No),
					     PLSAlertView::Button defaultButton = PLSAlertView::Button::NoButton);
	static void information(QWidget *parent, const QString &title, const QString &text);
	static void warning(QWidget *parent, const QString &title, const QString &text, bool enableRichText = false);
	static void critical(QWidget *parent, const QString &title, const QString &text);
};

void PLSErrorBox(QWidget *parent, const char *msg, ...);

void QTToGSWindow(WId windowId, gs_window &gswindow);

uint32_t TranslateQtKeyboardEventModifiers(Qt::KeyboardModifiers mods);

QDataStream &operator<<(QDataStream &out, const std::vector<std::shared_ptr<OBSSignal>> &signal_vec);
QDataStream &operator>>(QDataStream &in, std::vector<std::shared_ptr<OBSSignal>> &signal_vec);
QDataStream &operator<<(QDataStream &out, const OBSScene &scene);
QDataStream &operator>>(QDataStream &in, OBSScene &scene);
QDataStream &operator<<(QDataStream &out, const OBSSceneItem &si);
QDataStream &operator>>(QDataStream &in, OBSSceneItem &si);

QThread *CreateQThread(std::function<void()> func);

void ExecuteFuncSafeBlock(std::function<void()> func);
void ExecuteFuncSafeBlockMsgBox(std::function<void()> func, const QString &title, const QString &text);

/* allows executing without message boxes if starting up, otherwise with a
 * message box */
void EnableThreadedMessageBoxes(bool enable);
void ExecThreadedWithoutBlocking(std::function<void()> func, const QString &title, const QString &text);

class SignalBlocker {
	QWidget *widget;
	bool blocked;

public:
	inline explicit SignalBlocker(QWidget *widget_) : widget(widget_) { blocked = widget->blockSignals(true); }

	inline ~SignalBlocker() { widget->blockSignals(blocked); }
};

void DeleteLayout(QLayout *layout);

static inline Qt::ConnectionType WaitConnection()
{
	return QThread::currentThread() == qApp->thread() ? Qt::DirectConnection : Qt::BlockingQueuedConnection;
}

bool LineEditCanceled(QEvent *event);
bool LineEditChanged(QEvent *event);
