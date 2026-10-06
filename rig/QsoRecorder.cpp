#include <cmath>
#include <limits>

#include <QtMath>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QRegularExpression>

#include "QsoRecorder.h"
#include "digi/AudioInput.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.rig.qsorecorder");

#define DEFAULT_RATE         16000
#define DEFAULT_BUFFER_S     30
#define MAX_TAPS             511
// Blackman window: about 70 dB stopband
#define TAP_FACTOR           5.5
// passband edge as a share of the output rate; stopband starts at 0.5
#define PASSBAND_SHARE       0.4
#define WAV_HEADER_BYTES     44
#define FLUSH_BYTES          65536

/* With a voice input, mixing runs 1 s behind the rig so the gate can open
   before the CAT-polled PTT report arrived and the first word is not cut. */
#define MIX_DELAY_S          1.0
// gate widening around the reported TX, covers the CAT poll latency
#define GATE_LEAD_S          0.5
#define GATE_TAIL_S          0.4
// fade to avoid clicks at the gate
#define GAIN_RAMP_S          0.01
// two cards drift apart; voice ahead of the rig by more than this is dropped
#define MAX_VOICE_LEAD_S     0.25

/* ------------------------------------------------------------------ Reducer */

// integer decimation only; e.g. 44.1 kHz -> 16 kHz is refused
bool QsoRecorder::Reducer::design(int inputRate, int outputRate)
{
    FCT_IDENTIFICATION;

    taps.clear();
    history.clear();
    at = 0;
    since = 0;
    factor = 1;

    if ( inputRate <= 0 || outputRate <= 0 || inputRate % outputRate != 0 )
        return false;

    factor = inputRate / outputRate;

    if ( factor == 1 )
        return true;

    const double transition = ( 0.5 - PASSBAND_SHARE ) * outputRate;
    int count = static_cast<int>(std::ceil(TAP_FACTOR * inputRate / transition)) | 1;

    count = qBound(31, count, MAX_TAPS);

    const double cutoff = ( ( PASSBAND_SHARE + 0.5 ) / 2.0 ) * outputRate / inputRate;
    const int middle = count / 2;
    double sum = 0.0;

    taps.resize(count);

    for ( int i = 0; i < count; i++ )
    {
        const int n = i - middle;
        const double ideal = ( n == 0 ) ? 2.0 * cutoff
                                        : qSin(2.0 * M_PI * cutoff * n) / ( M_PI * n );
        const double window = 0.42 - 0.5 * qCos(2.0 * M_PI * i / ( count - 1 ))
                            + 0.08 * qCos(4.0 * M_PI * i / ( count - 1 ));

        taps[i] = ideal * window;
        sum += taps.at(i);
    }

    for ( double &tap : taps )
        tap /= sum;

    history.fill(0.0, count);
    return true;
}

void QsoRecorder::Reducer::push(const QVector<float> &in, QVector<float> &out)
{
    FCT_IDENTIFICATION;

    if ( factor <= 1 )
    {
        out += in;
        return;
    }

    const int size = taps.size();

    for ( float sample : in )
    {
        history[at] = sample;
        at = ( at + 1 == size ) ? 0 : at + 1;

        if ( ++since < factor )
            continue;

        since = 0;

        // symmetric filter: any starting point that covers the whole ring will do
        double sum = 0.0;
        int i = at;

        for ( int k = 0; k < size; k++ )
        {
            sum += taps.at(k) * history.at(i);

            if ( ++i == size )
                i = 0;
        }

        out.append(static_cast<float>(sum));
    }
}

/* ------------------------------------------------------------- QsoRecorder */

QsoRecorder::QsoRecorder(QObject *parent) :
    QObject(parent),
    audio(new AudioInput(this)),
    voiceAudio(new AudioInput(this)),
    stereoWanted(true),
    outputFolder(defaultFolder()),
    wantedRate(DEFAULT_RATE),
    bufferSeconds(DEFAULT_BUFFER_S),
    outputRate(0),
    channels(1),
    voiceOpen(false),
    mixDelay(0),
    rigSamplesIn(0),
    rigSamplesMixed(0),
    gateFrom(std::numeric_limits<qint64>::max()),
    gateTo(0),
    voiceGain(0.0),
    gainStep(1.0),
    lead(0),
    tail(0),
    ringAt(0),
    ringFrames(0),
    file(nullptr),
    dataBytes(0)
{
    FCT_IDENTIFICATION;

    connect(audio, &AudioInput::samplesReady, this, &QsoRecorder::rigArrived);
    connect(voiceAudio, &AudioInput::samplesReady, this, &QsoRecorder::voiceArrived);

    connect(audio, &AudioInput::failed, this, [this](const QString &reason)
    {
        // keep what was recorded so far
        if ( isRecording() )
            stop(QStringLiteral("interrupted"));

        emit failed(reason);
    });

    // losing the voice input does not end the recording; that channel goes quiet
    connect(voiceAudio, &AudioInput::failed, this, [this](const QString &reason)
    {
        emit failed(tr("Voice input: %1").arg(reason));
    });
}

QsoRecorder::~QsoRecorder()
{
    FCT_IDENTIFICATION;

    if ( isRecording() )
        stop(QStringLiteral("unfinished"));
}

QString QsoRecorder::defaultFolder()
{
    FCT_IDENTIFICATION;

    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
            .filePath(QStringLiteral("QLog Recordings"));
}

// settings apply on the next listen(), never to a recording in progress
void QsoRecorder::setDevice(const QString &name)
{
    FCT_IDENTIFICATION;
    deviceName = name;
}

void QsoRecorder::setVoiceDevice(const QString &name)
{
    FCT_IDENTIFICATION;
    voiceDeviceName = name;
}

void QsoRecorder::setStereo(bool on)
{
    FCT_IDENTIFICATION;
    stereoWanted = on;
}

void QsoRecorder::setFolder(const QString &path)
{
    FCT_IDENTIFICATION;
    outputFolder = path.isEmpty() ? defaultFolder() : path;
}

void QsoRecorder::setRate(int hz)
{
    FCT_IDENTIFICATION;
    wantedRate = ( hz > 0 ) ? hz : DEFAULT_RATE;
}

void QsoRecorder::setBufferSeconds(int seconds)
{
    FCT_IDENTIFICATION;
    bufferSeconds = qBound(0, seconds, 300);
}

bool QsoRecorder::isListening() const
{
    FCT_IDENTIFICATION;

    return audio->isRunning();
}

double QsoRecorder::recordedSeconds() const
{
    FCT_IDENTIFICATION;

    if ( outputRate <= 0 )
        return 0.0;

    return ( dataBytes + pending.size() ) / 2.0 / channels / outputRate;
}

/* A missing device is an error: AudioInput would silently fall back to the
   default input, which is the wrong source to record. */
bool QsoRecorder::listen()
{
    FCT_IDENTIFICATION;

    if ( isRecording() )
        return true;

    if ( deviceName.isEmpty() )
    {
        emit failed(tr("No recording input chosen"));
        return false;
    }

    voiceAudio->stop();
    voiceOpen = false;

    if ( !audio->start(deviceName) )
        return false;

    if ( audio->deviceName() != deviceName )
    {
        const QString found = audio->deviceName();

        audio->stop();
        emit failed(tr("%1 is not connected").arg(deviceName));
        qCDebug(runtime) << "wanted" << deviceName << "but the card offered" << found;
        return false;
    }

    const int rigRate = audio->sampleRate();

    // no integer factor to the wanted rate: keep the card's rate
    if ( !rigReducer.design(rigRate, wantedRate) )
        rigReducer.design(rigRate, rigRate);

    outputRate = rigRate / rigReducer.factor;

    if ( !voiceDeviceName.isEmpty() )
        voiceOpen = openVoice();

    channels = ( voiceOpen && stereoWanted ) ? 2 : 1;
    mixDelay = voiceOpen ? static_cast<int>(MIX_DELAY_S * outputRate) : 0;
    lead = static_cast<int>(GATE_LEAD_S * outputRate);
    tail = static_cast<int>(GATE_TAIL_S * outputRate);
    gainStep = 1.0 / qMax(1.0, GAIN_RAMP_S * outputRate);

    rigQueue.clear();
    voiceQueue.clear();
    rigSamplesIn = 0;
    rigSamplesMixed = 0;
    gateFrom = std::numeric_limits<qint64>::max();
    gateTo = 0;
    voiceGain = 0.0;

    ring.fill(0, outputRate * bufferSeconds * channels);
    ringAt = 0;
    ringFrames = 0;

    qCDebug(runtime) << "recording" << rigRate << "Hz kept at" << outputRate << "Hz,"
                     << channels << "channel(s), voice input" << voiceOpen
                     << "," << bufferSeconds << "s held back";
    return true;
}

// voice must decimate to exactly the rig's output rate, else it is left out
bool QsoRecorder::openVoice()
{
    FCT_IDENTIFICATION;

    if ( voiceDeviceName == deviceName )
    {
        emit failed(tr("The voice input is the rig's own card"));
        return false;
    }

    if ( !voiceAudio->start(voiceDeviceName) )
        return false;

    if ( voiceAudio->deviceName() != voiceDeviceName )
    {
        voiceAudio->stop();
        emit failed(tr("%1 is not connected").arg(voiceDeviceName));
        return false;
    }

    if ( !voiceReducer.design(voiceAudio->sampleRate(), outputRate) )
    {
        emit failed(tr("%1 runs at %2 Hz, which cannot be matched to %3 Hz")
                    .arg(voiceDeviceName).arg(voiceAudio->sampleRate()).arg(outputRate));
        voiceAudio->stop();
        return false;
    }

    return true;
}

void QsoRecorder::stopListening()
{
    FCT_IDENTIFICATION;

    if ( isRecording() )
        stop(QStringLiteral("unfinished"));

    audio->stop();
    voiceAudio->stop();
    voiceOpen = false;
}

// gate is in rig sample counts, widened both ways since the CAT report is late
void QsoRecorder::setTransmitting(bool on)
{
    FCT_IDENTIFICATION;

    if ( !voiceOpen )
        return;

    if ( on )
    {
        // PTT pressed again within the mix delay: extend the previous gate
        if ( !( gateTo >= 0 && rigSamplesMixed < gateTo ) )
            gateFrom = qMax<qint64>(0, rigSamplesIn - lead);

        gateTo = -1;
    }
    else if ( gateTo < 0 )
        gateTo = rigSamplesIn + tail;
}

void QsoRecorder::rigArrived(const QVector<float> &mono)
{
    FCT_IDENTIFICATION;

    const int before = rigQueue.size();

    rigReducer.push(mono, rigQueue);
    rigSamplesIn += rigQueue.size() - before;
    mix();
}

void QsoRecorder::voiceArrived(const QVector<float> &mono)
{
    FCT_IDENTIFICATION;

    if ( !voiceOpen )
        return;

    voiceReducer.push(mono, voiceQueue);

    const int excess = voiceQueue.size() - rigQueue.size()
                       - static_cast<int>(MAX_VOICE_LEAD_S * outputRate);

    if ( excess > 0 )
        voiceQueue.remove(0, excess);
}

void QsoRecorder::mix()
{
    FCT_IDENTIFICATION;

    const int count = rigQueue.size() - mixDelay;

    if ( count <= 0 )
        return;

    for ( int i = 0; i < count; i++ )
    {
        const double rig = rigQueue.at(i);
        double voice = 0.0;

        if ( voiceOpen )
        {
            const qint64 n = rigSamplesMixed;
            const bool open = ( n >= gateFrom ) && ( gateTo < 0 || n < gateTo );

            voiceGain = open ? qMin(1.0, voiceGain + gainStep)
                             : qMax(0.0, voiceGain - gainStep);

            if ( i < voiceQueue.size() )
                voice = voiceQueue.at(i) * voiceGain;
        }

        rigSamplesMixed++;

        auto toPcm = [](double value)
        {
            return static_cast<qint16>(qBound(-32767.0, value * 32767.0, 32767.0));
        };

        if ( channels == 2 )
            keep(toPcm(rig), toPcm(voice));
        else
            keep(toPcm(rig + voice), 0);
    }

    rigQueue.remove(0, count);
    voiceQueue.remove(0, qMin(count, static_cast<int>(voiceQueue.size())));

    if ( file && pending.size() >= FLUSH_BYTES )
    {
        if ( file->write(pending) != pending.size() )
        {
            abandon(tr("Cannot write %1").arg(tempPath));
            return;
        }

        dataBytes += pending.size();
        pending.clear();
    }
}

void QsoRecorder::keep(qint16 left, qint16 right)
{
    // no FCT_IDENTIFICATION: called once per sample
    if ( !ring.isEmpty() )
    {
        ring[ringAt * channels] = left;

        if ( channels == 2 )
            ring[ringAt * channels + 1] = right;

        ringAt = ( ( ringAt + 1 ) * channels == ring.size() ) ? 0 : ringAt + 1;
        ringFrames = qMin(ringFrames + 1, static_cast<int>(ring.size() / channels));
    }

    if ( file )
    {
        // WAV is little-endian
        pending.append(static_cast<char>(left & 0xff));
        pending.append(static_cast<char>(( left >> 8 ) & 0xff));

        if ( channels == 2 )
        {
            pending.append(static_cast<char>(right & 0xff));
            pending.append(static_cast<char>(( right >> 8 ) & 0xff));
        }
    }
}

// written empty at start (valid file) and again with real sizes at stop
bool QsoRecorder::writeHeader()
{
    FCT_IDENTIFICATION;

    if ( !file || !file->seek(0) )
        return false;

    const quint32 data = static_cast<quint32>(dataBytes);
    QByteArray header;

    auto u32 = [&header](quint32 value)
    {
        for ( int i = 0; i < 4; i++ )
            header.append(static_cast<char>(( value >> ( 8 * i ) ) & 0xff));
    };
    auto u16 = [&header](quint16 value)
    {
        header.append(static_cast<char>(value & 0xff));
        header.append(static_cast<char>(( value >> 8 ) & 0xff));
    };

    header.append("RIFF");
    u32(36 + data);
    header.append("WAVE");
    header.append("fmt ");
    u32(16);
    u16(1);                                                         // PCM
    u16(static_cast<quint16>(channels));
    u32(static_cast<quint32>(outputRate));
    u32(static_cast<quint32>(outputRate) * 2 * channels);           // bytes a second
    u16(static_cast<quint16>(2 * channels));                        // bytes a frame
    u16(16);                                                        // bits a sample
    header.append("data");
    u32(data);

    return file->write(header) == WAV_HEADER_BYTES && file->seek(file->size());
}

bool QsoRecorder::start()
{
    FCT_IDENTIFICATION;

    if ( isRecording() )
        return true;

    if ( !isListening() && !listen() )
        return false;

    if ( !QDir().mkpath(outputFolder) )
    {
        emit failed(tr("Cannot create %1").arg(outputFolder));
        return false;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();

    tempPath = QDir(outputFolder).filePath(
                QStringLiteral("~recording-%1.wav").arg(now.toString("yyyyMMdd-HHmmss")));
    file = new QFile(tempPath, this);

    if ( !file->open(QIODevice::WriteOnly | QIODevice::Truncate) )
    {
        delete file;
        file = nullptr;
        emit failed(tr("Cannot write %1").arg(tempPath));
        return false;
    }

    dataBytes = 0;
    pending.clear();

    if ( !writeHeader() )
    {
        abandon(tr("Cannot write %1").arg(tempPath));
        return false;
    }

    // ring buffer, oldest first
    const int frames = ring.size() / qMax(1, channels);
    const int oldest = ( ringFrames < frames ) ? 0 : ringAt;

    for ( int i = 0; i < ringFrames; i++ )
    {
        const int at = ( ( oldest + i ) % frames ) * channels;

        for ( int c = 0; c < channels; c++ )
        {
            const qint16 sample = ring.at(at + c);

            pending.append(static_cast<char>(sample & 0xff));
            pending.append(static_cast<char>(( sample >> 8 ) & 0xff));
        }
    }

    // start time = now minus held-back audio minus mix delay
    const qint64 behind = ( static_cast<qint64>(ringFrames) + mixDelay ) * 1000
                          / qMax(1, outputRate);

    startedAt = now.addMSecs(-behind);

    qCDebug(runtime) << "recording to" << tempPath << "with" << ringFrames
                     << "frames held back";
    emit recordingChanged(true);
    return true;
}

QString QsoRecorder::stop(const QString &label)
{
    FCT_IDENTIFICATION;

    if ( !isRecording() )
        return QString();

    if ( !pending.isEmpty() )
    {
        if ( file->write(pending) == pending.size() )
            dataBytes += pending.size();

        pending.clear();
    }

    writeHeader();
    file->close();
    delete file;
    file = nullptr;

    // <UTC start>_<label>.wav; unsafe characters (e.g. "/" in a call) become "-"
    QString safe = label.trimmed();

    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")), QStringLiteral("-"));
    safe.replace(QRegularExpression(QStringLiteral("^-+|-+$")), QString());

    const QString stem = startedAt.toString(QStringLiteral("yyyy-MM-dd_HHmm'Z'"))
                         + ( safe.isEmpty() ? QString() : QStringLiteral("_") + safe );
    QDir dir(outputFolder);
    QString target = dir.filePath(stem + QStringLiteral(".wav"));

    for ( int n = 2; QFile::exists(target); n++ )
        target = dir.filePath(QStringLiteral("%1_%2.wav").arg(stem).arg(n));

    if ( !QFile::rename(tempPath, target) )
    {
        qCWarning(runtime) << "cannot rename" << tempPath << "to" << target;
        target = tempPath;
    }

    qCDebug(runtime) << "recording saved:" << target << recordedSeconds() << "s";
    emit recordingChanged(false);
    emit saved(target);
    return target;
}

// the partial file is kept under its temporary name
void QsoRecorder::abandon(const QString &reason)
{
    FCT_IDENTIFICATION;

    qCWarning(runtime) << reason;

    if ( file )
    {
        file->close();
        delete file;
        file = nullptr;
    }

    pending.clear();
    emit recordingChanged(false);
    emit failed(reason);
}
