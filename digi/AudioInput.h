#ifndef QLOG_DIGI_AUDIOINPUT_H
#define QLOG_DIGI_AUDIOINPUT_H

#include <QObject>
#include <QAudioFormat>
#include <QElapsedTimer>
#include <QByteArray>
#include <QList>
#include <QVector>
#include <QStringList>

#include "digi/AudioFormat.h"
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
class QAudioSource;
#else
class QAudioInput;
#endif

class QIODevice;

/* Sound card capture delivering mono float samples in -1..1, whatever
   format the device actually accepted. */
class AudioInput : public QObject
{
    Q_OBJECT

public:
    explicit AudioInput(QObject *parent = nullptr);
    ~AudioInput();

    // names, not device objects: the device type differs between Qt 5 and 6
    static QStringList inputDeviceNames();

    bool start(const QString &description);
    void stop();

    bool isRunning() const { return source != nullptr; }
    // actual device rate, may differ from the requested one
    int sampleRate() const { return format.sampleRate(); }
    QString deviceName() const { return currentName; }

signals:
    void samplesReady(const QVector<float> &mono);
    void failed(const QString &reason);
    // measured samples/s; shows dropped buffers or a drifting sound clock
    void measuredRate(double hz);

private slots:
    void readAvailable();

private:
    void convert(const QByteArray &raw, QVector<float> &mono);
    DigiAudio::Sample sampleKind() const;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QAudioSource *source;
#else
    QAudioInput *source;
#endif
    QIODevice *io;
    QAudioFormat format;
    QString currentName;
    QByteArray remainder; // partial frame left over from the last read
    QElapsedTimer clock;
    qint64 framesSeen;
};

#endif // QLOG_DIGI_AUDIOINPUT_H
