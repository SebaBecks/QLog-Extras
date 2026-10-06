#ifndef QLOG_RIG_QSORECORDER_H
#define QLOG_RIG_QSORECORDER_H

#include <QObject>
#include <QVector>
#include <QString>
#include <QDateTime>

class AudioInput;
class QFile;

/* Records QSOs to WAV files (no DB changes). Keeps a pre-roll ring buffer so
   a recording starts with audio from before the button was pressed. Optional
   second input for the operator's voice, mixed in only while transmitting. */
class QsoRecorder : public QObject
{
    Q_OBJECT

public:
    explicit QsoRecorder(QObject *parent = nullptr);
    ~QsoRecorder();

    void setDevice(const QString &name);
    void setVoiceDevice(const QString &name); // empty for none
    // rig left / voice right, else mixed mono; only with a voice input
    void setStereo(bool on);
    void setFolder(const QString &path);
    void setRate(int hz);
    void setBufferSeconds(int seconds);

    QString folder() const { return outputFolder; }
    bool isListening() const;
    bool isRecording() const { return file != nullptr; }
    double recordedSeconds() const; // including the pre-roll
    int channelCount() const { return channels; }

    static QString defaultFolder();

public slots:
    bool listen();
    void stopListening();
    bool start();
    // label (call, band, mode) goes into the file name; returns the saved path
    QString stop(const QString &label);
    // rig TX state, gates the voice input
    void setTransmitting(bool on);

signals:
    void recordingChanged(bool on);
    void failed(const QString &reason);
    void saved(const QString &path);

private:
    // low-pass + integer decimation to the recording rate
    struct Reducer
    {
        int factor = 1;
        QVector<double> taps;
        QVector<double> history;
        int at = 0;
        int since = 0;

        bool design(int inputRate, int outputRate);
        void push(const QVector<float> &in, QVector<float> &out);
    };

    void rigArrived(const QVector<float> &mono);
    void voiceArrived(const QVector<float> &mono);
    void mix();
    void keep(qint16 left, qint16 right);
    bool writeHeader();
    void abandon(const QString &reason);
    bool openVoice();

    AudioInput *audio;
    AudioInput *voiceAudio;
    QString deviceName;
    QString voiceDeviceName;
    bool stereoWanted;
    QString outputFolder;
    int wantedRate;
    int bufferSeconds;

    int outputRate;
    int channels;
    bool voiceOpen;

    Reducer rigReducer;
    Reducer voiceReducer;

    // decimated audio waiting for the mix, which runs mixDelay behind the rig
    QVector<float> rigQueue;
    QVector<float> voiceQueue;
    int mixDelay;

    // voice gate in rig samples: [gateFrom, gateTo), gateTo -1 while still TX
    qint64 rigSamplesIn;
    qint64 rigSamplesMixed;
    qint64 gateFrom;
    qint64 gateTo;
    double voiceGain;
    double gainStep;
    int lead;
    int tail;

    // pre-roll, oldest frame at ringAt, interleaved when stereo
    QVector<qint16> ring;
    int ringAt;
    int ringFrames;

    QFile *file;
    QString tempPath;
    QDateTime startedAt;
    qint64 dataBytes;
    QByteArray pending;
};

#endif // QLOG_RIG_QSORECORDER_H
