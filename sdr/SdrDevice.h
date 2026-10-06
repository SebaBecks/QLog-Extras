#ifndef QLOG_SDR_SDRDEVICE_H
#define QLOG_SDR_SDRDEVICE_H

#include <QObject>
#include <QVector>
#include <QStringList>
#include <complex>

/* Base class for SDR receivers delivering raw I/Q samples.
   Drivers load their library at runtime, so QLog starts without it.
   samplesReady is emitted from the driver thread; samples are scaled to -1..1. */
class SdrDevice : public QObject
{
    Q_OBJECT

public:
    using Samples = QVector<std::complex<float>>;

    explicit SdrDevice(QObject *parent = nullptr) : QObject(parent) {}
    ~SdrDevice() override = default;

    static QStringList driverKeys();
    static QString driverName(const QString &key);
    /* nullptr for an unknown key. */
    static SdrDevice *create(const QString &key, QObject *parent = nullptr);

    /* Empty = driver default search. */
    virtual void setLibraryPath(const QString &path) = 0;
    virtual QString libraryPath() const = 0;

    /* Loads the library if needed; empty list if that fails. */
    virtual QStringList deviceNames() = 0;

    virtual bool open(int index) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    /* Model, tuner, serial; valid once open. */
    virtual QString description() const = 0;

    virtual bool setSampleRate(int hz) = 0;
    virtual int sampleRate() const = 0;
    virtual bool setCenterFrequency(double hz) = 0;
    virtual double centerFrequency() const = 0;
    /* dB; negative = device AGC. */
    virtual bool setGain(double db) = 0;
    /* Supported gains in dB, ascending. */
    virtual QVector<double> gains() const = 0;
    virtual bool setFrequencyCorrection(int ppm) = 0;
    /* HF without an upconverter; 0 = off. */
    virtual bool setDirectSampling(int mode)
    {
        return mode == 0 ? true : fail(tr("This receiver has no direct sampling"));
    }
    virtual bool setBiasTee(bool on)
    {
        return !on ? true : fail(tr("This receiver has no bias tee"));
    }

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    /* Reason for the last false return. */
    QString lastError() const { return error; }

signals:
    void samplesReady(const SdrDevice::Samples &samples);
    /* Device unplugged or stream broken while running. */
    void failed(const QString &reason);

protected:
    bool fail(const QString &reason)
    {
        error = reason;
        return false;
    }

    QString error;
};

Q_DECLARE_METATYPE(SdrDevice::Samples)

#endif // QLOG_SDR_SDRDEVICE_H
