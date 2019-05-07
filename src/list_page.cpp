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

#include <QAudioProbe>
#include <QDebug>
#include <QMediaPlayer>
#include <QVBoxLayout>
#include <QApplication>
#include <DHiDPIHelper>

#include "dimagebutton.h"
#include "list_page.h"
#include "utils.h"

DWIDGET_USE_NAMESPACE

ListPage::ListPage(QWidget *parent) : QWidget(parent)
{
    layout = new QVBoxLayout(this);
    setLayout(layout);

    fileView = new FileView(this);
    connect(fileView, &FileView::play, this, &ListPage::play);
    connect(fileView, &FileView::pause, this, &ListPage::pause);
    connect(fileView, &FileView::resume, this, &ListPage::resume);
    connect(fileView, &FileView::stop, this, &ListPage::stop);

    connect(this, &ListPage::playFinished, fileView, &FileView::handlePlayFinish);

    audioPlayer = new QMediaPlayer(this);

    connect(audioPlayer, SIGNAL(stateChanged(QMediaPlayer::State)), this, SLOT(handleStateChanged(QMediaPlayer::State)));
    audioProbe = new QAudioProbe(this);
    if (audioProbe->setSource(audioPlayer)) {
        connect(audioProbe, SIGNAL(audioBufferProbed(QAudioBuffer)), this, SLOT(renderLevel(QAudioBuffer)));
    }

    waveform = new Waveform(this);
    waveform->hide();
    recordButton = new DImageButton(
        Utils::getQrcPath("record_small_normal.svg"),
        Utils::getQrcPath("record_small_hover.svg"),
        Utils::getQrcPath("record_small_press.svg")
        );
    
    connect(recordButton, SIGNAL(clicked()), this, SLOT(handleClickRecordButton()));

    layout->addWidget(fileView, 0, Qt::AlignHCenter);
    layout->addStretch();
    layout->addWidget(waveform, 0, Qt::AlignCenter);
    layout->addStretch();
    layout->addWidget(recordButton, 0, Qt::AlignHCenter);
    layout->addSpacing(21);     // NOTE: bottom buttons padding
}

void ListPage::handleClickRecordButton()
{
    // Must stop player before new record.
    audioPlayer->stop();
        
    emit clickRecordButton();
}

void ListPage::play(QString filepath)
{
    if (filepath != getPlayingFilepath()) {
        audioPlayer->stop();
    }

    waveform->show();

    audioPlayer->setMedia(QUrl::fromLocalFile(filepath));
    audioPlayer->play();
}

void ListPage::pause(QString)
{
    audioPlayer->pause();
}

void ListPage::resume(QString)
{
    audioPlayer->play();
}

void ListPage::stop(QString filepath)
{
    if (filepath == getPlayingFilepath()) {
        audioPlayer->stop();
    }
}

void ListPage::stopPlayer()
{
    audioPlayer->stop();
}

void ListPage::renderLevel(const QAudioBuffer &buffer)
{
    QVector<qreal> levels = Waveform::getBufferLevels(buffer);
    for (int i = 0; i < levels.count(); ++i) {
        waveform->updateWave(levels.at(i));
    }
}

void ListPage::handleStateChanged(QMediaPlayer::State state)
{
    if (state == QMediaPlayer::StoppedState) {
        emit playFinished(getPlayingFilepath());

        waveform->hide();
        waveform->clearWave();
    }
}

QString ListPage::getPlayingFilepath()
{
    if (audioPlayer->isAudioAvailable()) {
        return audioPlayer->media().resources().first().url().path();
    } else {
        return "";
    }
}

void ListPage::selectItemWithPath(QString path)
{
    fileView->selectItemWithPath(path);
}
