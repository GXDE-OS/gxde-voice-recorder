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

#include <QAudioEncoderSettings>
#include <QAudioRecorder>
#include <QDate>
#include <QDebug>
#include <QWidget>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QStandardPaths>
#include <QTime>
#include <QTimer>
#include <QtEndian>
#include <QUrl>
#include <QVBoxLayout>
#include <QApplication>
#include <DHiDPIHelper>
#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#ifdef __cplusplus
}
#endif

#include <chrono>

#include "dimagebutton.h"
#include "record_page.h"
#include "recording_button.h"
#include "utils.h"
#include "waveform.h"

DWIDGET_USE_NAMESPACE

static int audioLevelMonitorInterruptCb(void *opaque)
{
    auto *monitor = static_cast<AudioLevelMonitor *>(opaque);
    return monitor->isRunning() ? 0 : 1;  
}

static qreal computeFramePeak(AVFrame *frame, int channels)
{
    int samples = frame->nb_samples;
    qreal peak = 0.0;

    switch (frame->format) {
    case AV_SAMPLE_FMT_S16: {
        const int16_t *d = reinterpret_cast<const int16_t *>(frame->data[0]);
        for (int i = 0; i < samples * channels; ++i) {
            qreal v = qAbs(qreal(d[i]) / SHRT_MAX);
            if (v > peak) peak = v;
        }
        break;
    }
    case AV_SAMPLE_FMT_S32: {
        const int32_t *d = reinterpret_cast<const int32_t *>(frame->data[0]);
        for (int i = 0; i < samples * channels; ++i) {
            qreal v = qAbs(qreal(d[i]) / INT_MAX);
            if (v > peak) peak = v;
        }
        break;
    }
    case AV_SAMPLE_FMT_FLT: {
        const float *d = reinterpret_cast<const float *>(frame->data[0]);
        for (int i = 0; i < samples * channels; ++i) {
            qreal v = qAbs(qreal(d[i]));
            if (v > peak) peak = v;
        }
        break;
    }
    case AV_SAMPLE_FMT_FLTP: {
        for (int c = 0; c < channels; ++c) {
            const float *d = reinterpret_cast<const float *>(frame->data[c]);
            for (int i = 0; i < samples; ++i) {
                qreal v = qAbs(qreal(d[i]));
                if (v > peak) peak = v;
            }
        }
        break;
    }
    default:
        break;
    }

    return peak;
}

AudioLevelMonitor::AudioLevelMonitor(QObject *parent)
    : QObject(parent), recordingTimer(nullptr),
      latestPeak(-1.0), running(false), paused(false), recordingMode(false)
{
}

AudioLevelMonitor::~AudioLevelMonitor()
{
    stop();
}

void AudioLevelMonitor::start()
{
    if (running.load()) {
        return;
    }
    recordingMode = true;
    running.store(true);
    paused.store(false);
    latestPeak.store(-1.0);

    workerThread = std::thread(&AudioLevelMonitor::runDeviceLoop, this);

    recordingTimer = new QTimer(this);
    connect(recordingTimer, &QTimer::timeout, this, &AudioLevelMonitor::onRecordingTimer);
    recordingTimer->start(10);  
}

void AudioLevelMonitor::startFile(const QString &path)
{
    if (running.load()) {
        stop();
    }
    fileSource = path;
    recordingMode = false;
    running.store(true);
    paused.store(false);
    workerThread = std::thread(&AudioLevelMonitor::runLoop, this);
}

void AudioLevelMonitor::stop()
{
    running.store(false);

    if (recordingTimer) {
        recordingTimer->stop();
        delete recordingTimer;
        recordingTimer = nullptr;
    }

    if (workerThread.joinable()) {
        workerThread.join();
    }
}

void AudioLevelMonitor::pause()
{
    paused.store(true);
}

void AudioLevelMonitor::resume()
{
    paused.store(false);
}

// Main-thread timer (10ms). Atomically reads and resets latestPeak, then
// emits levelReady. This avoids cross-thread signal delivery issues.
void AudioLevelMonitor::onRecordingTimer()
{
    if (!running.load() || paused.load()) {
        return;
    }
    qreal peak = latestPeak.exchange(-1.0);  // read and reset
    if (peak >= 0.0) {
        emit levelReady(peak);
    }
}

void AudioLevelMonitor::runDeviceLoop()
{
    avdevice_register_all();
    AVFormatContext *fmtCtx = avformat_alloc_context();
    fmtCtx->interrupt_callback.callback = audioLevelMonitorInterruptCb;
    fmtCtx->interrupt_callback.opaque = this;

    int ret = -1;

    // 尝试 ALSA
    const AVInputFormat *alsaFmt = av_find_input_format("alsa");
    if (alsaFmt) {
        AVDictionary *options = nullptr;
        av_dict_set(&options, "sample_rate", "44100", 0);
        av_dict_set(&options, "channels", "1", 0);
        av_dict_set(&options, "fragment_size", "1024", 0);

        ret = avformat_open_input(&fmtCtx, "default", alsaFmt, &options);
        av_dict_free(&options);

        if (ret < 0) {
            qDebug() << "AudioLevelMonitor: ALSA 'default' failed, trying PulseAudio...";
        }
    } else {
        qDebug() << "AudioLevelMonitor: FFmpeg has no ALSA support";
    }

    // 回退到 PulseAudio
    if (ret < 0) {
        const AVInputFormat *pulseFmt = av_find_input_format("pulse");
        if (pulseFmt) {
            AVDictionary *options = nullptr;
            av_dict_set(&options, "sample_rate", "44100", 0);
            av_dict_set(&options, "channels", "1", 0);
            av_dict_set(&options, "fragment_size", "1024", 0);

            ret = avformat_open_input(&fmtCtx, "default", pulseFmt, &options);
            av_dict_free(&options);

            if (ret < 0) {
                qDebug() << "AudioLevelMonitor: PulseAudio 'default' also failed";
            }
        } else {
            qDebug() << "AudioLevelMonitor: FFmpeg has no PulseAudio support";
        }
    }

    if (ret < 0) {
        qDebug() << "AudioLevelMonitor: cannot open any audio device for level monitoring";
        avformat_free_context(fmtCtx);
        return;
    }

    qDebug() << "AudioLevelMonitor: device opened successfully";

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return;
    }

    int audioStreamIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIdx = i;
            break;
        }
    }
    if (audioStreamIdx < 0) {
        avformat_close_input(&fmtCtx);
        return;
    }

    AVCodecParameters *codecPar = fmtCtx->streams[audioStreamIdx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec) {
        avformat_close_input(&fmtCtx);
        return;
    }
    AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, codecPar);
    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return;
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    while (running.load() && !paused.load()) {
        ret = av_read_frame(fmtCtx, packet);
        if (ret < 0) {
            break;
        }

        if (packet->stream_index == audioStreamIdx) {
            ret = avcodec_send_packet(codecCtx, packet);
            while (ret >= 0) {
                ret = avcodec_receive_frame(codecCtx, frame);
                if (ret < 0) {
                    break;
                }

                int channels = codecCtx->ch_layout.nb_channels > 0
                                   ? codecCtx->ch_layout.nb_channels
                                   : 1;
                qreal peak = computeFramePeak(frame, channels);
                // Store the maximum peak since last timer poll.
                qreal prev = latestPeak.load();
                while (peak > prev && !latestPeak.compare_exchange_weak(prev, peak)) {}
            }
        }
        av_packet_unref(packet);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);
}

void AudioLevelMonitor::runFileLoop(AVFormatContext *fmtCtx, int audioStreamIdx)
{
    AVCodecContext *codecCtx = nullptr;
    // Stream already located by runLoop(); just open the codec.
    AVCodecParameters *codecPar = fmtCtx->streams[audioStreamIdx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec) {
        return;
    }
    codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, codecPar);
    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        return;
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    AVRational timeBase = fmtCtx->streams[audioStreamIdx]->time_base;
    auto playbackStart = std::chrono::steady_clock::now();
    int64_t startPtsUs = AV_NOPTS_VALUE;
    int64_t pausedAccumUs = 0;
    std::chrono::steady_clock::time_point pauseStart{};

    while (running.load()) {
        if (paused.load()) {
            if (pauseStart == std::chrono::steady_clock::time_point{}) {
                pauseStart = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        // Accumulate pause duration once on resume.
        if (pauseStart != std::chrono::steady_clock::time_point{}) {
            pausedAccumUs += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - pauseStart).count();
            pauseStart = {};
        }

        int ret = av_read_frame(fmtCtx, packet);
        if (ret < 0) {
            break;  // EOF or error: playback finished
        }

        if (packet->stream_index == audioStreamIdx) {
            ret = avcodec_send_packet(codecCtx, packet);
            while (ret >= 0) {
                ret = avcodec_receive_frame(codecCtx, frame);
                if (ret < 0) {
                    break;
                }

                // Pace output by the frame's presentation timestamp so the
                // waveform advances in sync with real-time playback.
                if (frame->pts != AV_NOPTS_VALUE) {
                    int64_t ptsUs = av_rescale_q(frame->pts, timeBase, {1, 1000000});
                    if (startPtsUs == AV_NOPTS_VALUE) {
                        startPtsUs = ptsUs;
                    }
                    int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - playbackStart).count() - pausedAccumUs;
                    int64_t waitUs = (ptsUs - startPtsUs) - elapsedUs;
                    if (waitUs > 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(waitUs));
                    }
                }

                int channels = codecCtx->ch_layout.nb_channels > 0
                                   ? codecCtx->ch_layout.nb_channels
                                   : 1;
                qreal peak = computeFramePeak(frame, channels);
                emit levelReady(peak);
            }
        }
        av_packet_unref(packet);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codecCtx);
}

void AudioLevelMonitor::runLoop()
{
    AVFormatContext *fmtCtx = avformat_alloc_context();
    if (!fmtCtx) {
        return;
    }
    fmtCtx->interrupt_callback.callback = audioLevelMonitorInterruptCb;
    fmtCtx->interrupt_callback.opaque = this;

    QByteArray pathBytes = fileSource.toUtf8();
    if (avformat_open_input(&fmtCtx, pathBytes.constData(), nullptr, nullptr) < 0) {
        avformat_free_context(fmtCtx);
        qDebug() << "AudioLevelMonitor: failed to open file" << fileSource;
        return;
    }

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return;
    }
    int audioStreamIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIdx = i;
            break;
        }
    }
    if (audioStreamIdx < 0) {
        avformat_close_input(&fmtCtx);
        return;
    }

    runFileLoop(fmtCtx, audioStreamIdx);
    avformat_close_input(&fmtCtx);
}

RecordPage::RecordPage(QWidget *parent) : QWidget(parent)
{
    installEventFilter(this);  // add event filter

    recordingTime = 0;

    layout = new QVBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    setLayout(layout);

    titleLabel = new QLabel(tr("New recording"));
    QFont titleFont;
    titleFont.setPixelSize(26);
    titleLabel->setFont(titleFont);
    waveform = new Waveform(this);
    QFont recordTimeFont;
    recordTimeFont.setPixelSize(14);
    recordTimeLabel = new QLabel("00:00");
    recordTimeLabel->setFont(recordTimeFont);

    buttonAreaWidget = new QWidget();
    buttonAreaLayout = new QVBoxLayout();
    buttonAreaLayout->setContentsMargins(0, 0, 0, 0);

    buttonWidget = new QWidget();
    buttonLayout = new QHBoxLayout(buttonWidget);
    buttonLayout->setContentsMargins(0, 0, 0, 0);

    expandAnimationButtonLayout = new QVBoxLayout();
    expandAnimationButtonLayout->setContentsMargins(0, 0, 0, 0);

    expandAnimationButton = new ExpandAnimationButton(this);
    connect(expandAnimationButton, &ExpandAnimationButton::finish, this, &RecordPage::handleExpandAnimationFinish);

    expandAnimationButtonLayout->addWidget(expandAnimationButton, 0, Qt::AlignHCenter);

    shrankAnimationButtonLayout = new QVBoxLayout();
    shrankAnimationButtonLayout->setContentsMargins(0, 0, 0, 0);

    shrankAnimationButton = new ShrankAnimationButton();
    connect(shrankAnimationButton, &ShrankAnimationButton::finish, this, &RecordPage::handleShrankAnimationFinish);

    shrankAnimationButtonLayout->addWidget(shrankAnimationButton, 0, Qt::AlignHCenter);

    recordingButton = new RecordingButton();

    finishButton = new DImageButton(
        Utils::getQrcPath("finish_normal.svg"),
        Utils::getQrcPath("finish_hover.svg"),
        Utils::getQrcPath("finish_press.svg")
    );

    buttonLayout->addStretch();
    buttonLayout->addWidget(recordingButton, 0, Qt::AlignVCenter);
    buttonLayout->addSpacing(20);
    buttonLayout->addWidget(finishButton, 0, Qt::AlignVCenter);
    buttonLayout->addStretch();

    buttonAreaLayout->addWidget(buttonWidget, 0, Qt::AlignHCenter);

    layout->addSpacing(36);
    layout->addWidget(titleLabel, 0, Qt::AlignHCenter);
    layout->addStretch();
    layout->addWidget(waveform, 1, Qt::AlignHCenter);
    layout->addStretch();
    layout->addWidget(recordTimeLabel, 0, Qt::AlignHCenter);
    layout->addStretch();
    layout->addWidget(buttonAreaWidget);
    layout->addSpacing(30);     // NOTE: bottom buttons padding

    audioRecorder = new QAudioRecorder(this);
    qDebug() << "support codecs:" << audioRecorder->supportedAudioCodecs();
    qDebug() << "support containers:" << audioRecorder->supportedContainers();

    QAudioEncoderSettings audioSettings;
    audioSettings.setQuality(QMultimedia::HighQuality);

#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    audioRecorder->setAudioSettings(audioSettings);
    audioRecorder->setContainerFormat("audio/x-wav");
#else
    audioSettings.setCodec("audio/PCM");
    audioRecorder->setAudioSettings(audioSettings);
    audioRecorder->setContainerFormat("wav");
#endif

    // FFmpeg-based level monitor replaces QAudioProbe (deprecated/removed in newer Qt).
    audioLevelMonitor = new AudioLevelMonitor(this);
    connect(audioLevelMonitor, &AudioLevelMonitor::levelReady, this, &RecordPage::renderLevel);

    tickerTimer = new QTimer(this);
    connect(tickerTimer, SIGNAL(timeout()), this, SLOT(renderRecordingTime()));
    tickerTimer->start(1000);

    startRecord();

    connect(finishButton, SIGNAL(clicked()), this, SLOT(handleClickFinishButton()));
    connect(recordingButton, SIGNAL(pause()), this, SLOT(pauseRecord()));
    connect(recordingButton, SIGNAL(resume()), this, SLOT(resumeRecord()));

    QFileInfoList fileInfoList = Utils::getRecordingFileinfos();
    if (fileInfoList.count() == 0) {
        buttonAreaWidget->setLayout(buttonAreaLayout);

        // Get keyboard focus.
        setFocus();
    } else {
        buttonAreaWidget->setLayout(expandAnimationButtonLayout);
        expandAnimationButton->startAnimation();
    }
}

void RecordPage::handleExpandAnimationFinish()
{
    Utils::removeChildren(buttonAreaWidget);

    buttonAreaWidget->setLayout(buttonAreaLayout);

    // Get keyboard focus.
    setFocus();
}

void RecordPage::handleShrankAnimationFinish()
{
    emit finishRecord(getRecordingFilepath());
}

void RecordPage::handleClickFinishButton()
{
    stopRecord();

    Utils::removeChildren(buttonAreaWidget);

    buttonAreaWidget->setLayout(shrankAnimationButtonLayout);
    shrankAnimationButton->startAnimation();
}

void RecordPage::renderRecordingTime()
{
    if (audioRecorder->state() != QMediaRecorder::StoppedState) {
        recordTimeLabel->setText(Utils::formatMillisecond(recordingTime));
    }
}

void RecordPage::startRecord()
{
    recordPath = generateRecordingFilepath();
    audioRecorder->setOutputLocation(recordPath);

    QDateTime currentTime = QDateTime::currentDateTime();
    lastUpdateTime = currentTime;
    audioRecorder->record();
    audioLevelMonitor->start();
}

void RecordPage::stopRecord()
{
    audioRecorder->stop();
    audioLevelMonitor->stop();
    tickerTimer->stop();
}

void RecordPage::exitRecord()
{
    stopRecord();

    QFile(getRecordingFilepath()).remove();

    emit cancelRecord();
}

void RecordPage::pauseRecord()
{
    audioRecorder->pause();
    audioLevelMonitor->pause();
}

void RecordPage::resumeRecord()
{
    QDateTime currentTime = QDateTime::currentDateTime();
    lastUpdateTime = currentTime;

    audioRecorder->record();
    audioLevelMonitor->resume();
}

QString RecordPage::generateRecordingFilepath()
{
    return QDir(Utils::getRecordingSaveDirectory()).filePath(QString("%1 (%2).wav").arg(tr("New recording")).arg(QDateTime::currentDateTime().toString("yyyyMMddhhmmss")));
}

QString RecordPage::getRecordingFilepath()
{
    return recordPath;
}

void RecordPage::renderLevel(qreal level)
{
    qreal mapped = pow(level, 0.8); 
    if (mapped > 1.0) mapped = 1.0;

    QDateTime currentTime = QDateTime::currentDateTime();
    recordingTime += lastUpdateTime.msecsTo(currentTime);
    lastUpdateTime = currentTime;

    waveform->updateWave(mapped);
}

bool RecordPage::eventFilter(QObject *, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);

        if (keyEvent == QKeySequence::Cancel) {
            exitRecord();
        }
    }

    return false;
}
