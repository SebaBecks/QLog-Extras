#include <QIODevice>
#include <QElapsedTimer>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioDevice>
#include <QAudioSource>
#include <QMediaDevices>
#else
#include <QAudioDeviceInfo>
#include <QAudioInput>
#endif

#include "AudioInput.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.digi.audioinput");

// Qt 5 describes a sample by size and signedness, Qt 6 by one enum
DigiAudio::Sample AudioInput::sampleKind() const
{
    FCT_IDENTIFICATION;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    switch ( format.sampleFormat() )
    {
    case QAudioFormat::UInt8: return DigiAudio::Sample::UInt8;
    case QAudioFormat::Int32: return DigiAudio::Sample::Int32;
    case QAudioFormat::Float: return DigiAudio::Sample::Float;
    default:                  return DigiAudio::Sample::Int16;
    }
#else
    if ( format.sampleType() == QAudioFormat::Float )
        return DigiAudio::Sample::Float;

    if ( format.sampleType() == QAudioFormat::UnSignedInt && format.sampleSize() == 8 )
        return DigiAudio::Sample::UInt8;

    if ( format.sampleSize() == 32 )
        return DigiAudio::Sample::Int32;

    return DigiAudio::Sample::Int16;
#endif
}

#define PREFERRED_RATE   48000
// rate window; 10 s keeps chunking error around 0.1 %, below a real loss
#define MEASURE_SECONDS  10.0
/* Reading shares the GUI thread with the waterfall. Qt's default buffer
   overran during painting (1-2 % of samples lost), so allow 0.5 s. */
#define BUFFER_MICROSECONDS 500000

AudioInput::AudioInput(QObject *parent) :
    QObject(parent),
    source(nullptr),
    io(nullptr),
    framesSeen(0)
{
    FCT_IDENTIFICATION;
}

AudioInput::~AudioInput()
{
    FCT_IDENTIFICATION;
    stop();
}

namespace
{

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)

QList<QAudioDevice> devices()
{
    FCT_IDENTIFICATION;

    return QMediaDevices::audioInputs();
}

QAudioDevice deviceNamed(const QString &description)
{
    FCT_IDENTIFICATION;

    const QList<QAudioDevice> found = devices();

    for ( const QAudioDevice &device : found )
        if ( device.description() == description )
            return device;

    // device gone (e.g. USB rig switched off): fall back to the default
    return QMediaDevices::defaultAudioInput();
}

QString nameOf(const QAudioDevice &device)
{
    FCT_IDENTIFICATION;

    return device.description();
}

QAudioFormat wanted()
{
    FCT_IDENTIFICATION;

    QAudioFormat format;

    format.setSampleRate(PREFERRED_RATE);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    return format;
}

#else

QList<QAudioDeviceInfo> devices()
{
    FCT_IDENTIFICATION;

    return QAudioDeviceInfo::availableDevices(QAudio::AudioInput);
}

QAudioDeviceInfo deviceNamed(const QString &description)
{
    FCT_IDENTIFICATION;

    const QList<QAudioDeviceInfo> found = devices();

    for ( const QAudioDeviceInfo &device : found )
        if ( device.deviceName() == description )
            return device;

    return QAudioDeviceInfo::defaultInputDevice();
}

QString nameOf(const QAudioDeviceInfo &device)
{
    FCT_IDENTIFICATION;

    return device.deviceName();
}

QAudioFormat wanted()
{
    FCT_IDENTIFICATION;

    QAudioFormat format;

    format.setSampleRate(PREFERRED_RATE);
    format.setChannelCount(1);
    format.setSampleSize(16);
    format.setSampleType(QAudioFormat::SignedInt);
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setCodec(QStringLiteral("audio/pcm"));

    return format;
}

#endif

} // namespace

QStringList AudioInput::inputDeviceNames()
{
    FCT_IDENTIFICATION;

    QStringList names;

    const auto found = devices();

    for ( const auto &device : found )
        names.append(nameOf(device));

    return names;
}

bool AudioInput::start(const QString &description)
{
    FCT_IDENTIFICATION;

    stop();

    const auto device = deviceNamed(description);
    const QString found = nameOf(device);

    if ( found.isEmpty() )
    {
        emit failed(tr("No audio input device"));
        return false;
    }

    format = wanted();

    // some cards refuse 48 kHz mono; take their format and convert
    if ( !device.isFormatSupported(format) )
    {
        format = device.preferredFormat();
        qCDebug(runtime) << "device refused the asked format, using"
                         << format.sampleRate() << "Hz"
                         << format.channelCount() << "channels";
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    source = new QAudioSource(device, format, this);
#else
    source = new QAudioInput(device, format, this);
#endif
    source->setBufferSize(format.bytesForDuration(BUFFER_MICROSECONDS));
    io = source->start();

    if ( !io )
    {
        qCWarning(runtime) << "cannot open audio input:" << source->error();
        emit failed(tr("Cannot open %1").arg(found));
        delete source;
        source = nullptr;
        return false;
    }

    currentName = found;
    remainder.clear();
    framesSeen = 0;
    clock.start();

    connect(io, &QIODevice::readyRead, this, &AudioInput::readAvailable);

    qCDebug(runtime) << "audio input open:" << currentName
                     << format.sampleRate() << "Hz"
                     << format.channelCount() << "channels";
    return true;
}

void AudioInput::stop()
{
    FCT_IDENTIFICATION;

    if ( !source )
        return;

    source->stop();
    source->deleteLater();
    source = nullptr;
    io = nullptr;
    remainder.clear();
    currentName.clear();
}

void AudioInput::readAvailable()
{
    FCT_IDENTIFICATION;

    if ( !io )
        return;

    QVector<float> mono;

    convert(remainder + io->readAll(), mono);

    if ( mono.isEmpty() )
        return;

    emit samplesReady(mono);

    framesSeen += mono.size();

    const double elapsed = clock.elapsed() / 1000.0;

    if ( elapsed >= MEASURE_SECONDS )
    {
        emit measuredRate(framesSeen / elapsed);
        framesSeen = 0;
        clock.restart();
    }
}

/* Converts to mono floats in -1..1. Channels are averaged so a rig wired
   to only one side of a stereo input is still heard. */
void AudioInput::convert(const QByteArray &raw, QVector<float> &mono)
{
    FCT_IDENTIFICATION;

    const int channels = qMax(1, format.channelCount());
    const int bytesPerSample = DigiAudio::bytesPerSample(sampleKind());

    if ( bytesPerSample <= 0 )
        return;

    const int frameBytes = bytesPerSample * channels;
    const int frames = raw.size() / frameBytes;

    // keep a trailing partial frame for the next read
    remainder = raw.mid(frames * frameBytes);

    if ( frames <= 0 )
        return;

    mono.resize(frames);

    const char *cursor = raw.constData();

    for ( int frame = 0; frame < frames; frame++ )
    {
        double sum = 0.0;

        for ( int channel = 0; channel < channels; channel++ )
        {
            const char *at = cursor + ( frame * channels + channel ) * bytesPerSample;

            switch ( sampleKind() )
            {
            case DigiAudio::Sample::UInt8:
                sum += ( static_cast<double>(*reinterpret_cast<const quint8 *>(at)) - 128.0 ) / 128.0;
                break;

            case DigiAudio::Sample::Int32:
                sum += *reinterpret_cast<const qint32 *>(at) / 2147483648.0;
                break;

            case DigiAudio::Sample::Float:
                sum += *reinterpret_cast<const float *>(at);
                break;

            default:
                sum += *reinterpret_cast<const qint16 *>(at) / 32768.0;
                break;
            }
        }

        mono[frame] = static_cast<float>(sum / channels);
    }
}
