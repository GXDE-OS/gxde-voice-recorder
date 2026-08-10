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

#include <QDebug>
#include <QMediaPlayer>
#include <QVBoxLayout>
#include <QApplication>
#include <DHiDPIHelper>

#include "dimagebutton.h"
#include "list_page.h"
#include "record_page.h"  
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
    audioOutput = new QAudioOutput(this);
    audioPlayer->setAudioOutput(audioOutput);

    connect(audioPlayer, SIGNAL(stateChanged(QMediaPlayer::State)), this, SLOT(handleStateChanged(QMediaPlayer::State)));

    // FFmpeg-based level monitor replaces QAudioProbe (deprecated/removed in newer Qt).
    audioLevelMonitor = new AudioLevelMonitor(this);
    connect(audioLevelMonitor, &AudioLevelMonitor::levelReady, this, &ListPage::renderLevel);

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
    audioLevelMonitor->stop();

    emit clickRecordButton();
}

void ListPage::play(QString filepath)
{
    if (filepath != getPlayingFilepath()) {
        audioPlayer->stop();
        audioLevelMonitor->stop();
    }

    waveform->show();
    waveform->clearWave();

    audioPlayer->setSource(QUrl::fromLocalFile(filepath));
    audioPlayer->play();
    audioLevelMonitor->startFile(filepath);
}

void ListPage::pause(QString)
{
    audioPlayer->pause();
    audioLevelMonitor->pause();
}

void ListPage::resume(QString)
{
    audioPlayer->play();
    audioLevelMonitor->resume();
}

void ListPage::stop(QString filepath)
{
    if (filepath == getPlayingFilepath()) {
        audioPlayer->stop();
        audioLevelMonitor->stop();
    }
}

void ListPage::stopPlayer()
{
    audioPlayer->stop();
    audioLevelMonitor->stop();
}

void ListPage::renderLevel(qreal level)
{
    waveform->updateWave(level);
}

void ListPage::handleStateChanged(QMediaPlayer::PlaybackState state)
{
    if (state == QMediaPlayer::StoppedState) {
        audioLevelMonitor->stop();
        emit playFinished(getPlayingFilepath());

        waveform->hide();
        waveform->clearWave();
    }
}

QString ListPage::getPlayingFilepath()
{
    if (audioPlayer->hasAudio()) {
        return audioPlayer->source().toLocalFile();
    } else {
        return "";
    }
}

void ListPage::selectItemWithPath(QString path)
{
    fileView->selectItemWithPath(path);
}
