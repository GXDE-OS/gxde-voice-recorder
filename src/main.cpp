/* -*- Mode: C++; indent-tabs-mode: nil; tab-width: 4 -*-
 * -*- coding: utf-8 -*-
 *
 * Copyright (C) 2011 ~ 2018 Deepin, Inc.
 *               2011 ~ 2018 Wang Yong
 *
 * Author:     Wang Yong <wangyong@deepin.com>
 * Maintainer: Wang Yong <wangyong@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <DApplication>
#include <DMainWindow>
#include <QApplication>
#include <QDesktopWidget>
#include <DWidgetUtil>
#include <DHiDPIHelper>

#include "main_window.h"
#include "utils.h"

DWIDGET_USE_NAMESPACE

int main(int argc, char *argv[])
{
    DApplication::loadDXcbPlugin();

    const char *descriptionText =
        QT_TRANSLATE_NOOP("MainWindow",
                          "GXDE Voice Recorder is a simple, beautiful and easy to use "
                          "voice recording application. It supports visual recording, "
                          "playback, recordings management and other functions.");

    const QString acknowledgementLink = "https://gxde.gfdgdxi.top";

    DApplication app(argc, argv);
    app.setAttribute(Qt::AA_UseHighDpiPixmaps);

    if (app.setSingleInstance("gxde-voice-recorder")) {
        app.loadTranslator();

        app.setOrganizationName("GXDE");
        app.setApplicationName("gxde-voice-recorder");
        app.setApplicationVersion(DApplication::buildVersion("1.0"));

        app.setProductIcon(QIcon(Utils::getQrcPath("logo_96.svg")));
        app.setProductName(DApplication::translate("MainWindow", "GXDE Voice Recorder"));
        app.setApplicationDescription(DApplication::translate("MainWindow", descriptionText) + "\n");
        app.setApplicationAcknowledgementPage(acknowledgementLink);

        app.setTheme("light");
        app.setWindowIcon(QIcon(Utils::getQrcPath("gxde-voice-recorder.svg")));

        MainWindow window;

        QObject::connect(&app, &DApplication::newInstanceStarted, &window, &MainWindow::activateWindow);

        Utils::applyQss(&window, "main.qss");
        Dtk::Widget::moveToCenter(&window);
        window.show();

        return app.exec();
    }

    return 0;
}
