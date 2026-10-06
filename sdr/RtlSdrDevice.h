#ifndef QLOG_SDR_RTLSDRDEVICE_H
#define QLOG_SDR_RTLSDRDEVICE_H

#include <QLibrary>
#include <atomic>
#include <thread>

#include "SdrDevice.h"

/* RTL-SDR dongle via librtlsdr, loaded at runtime.
   Blog V4 needs the RTL-SDR Blog library (R828D, HF upconverter below
   28.8 MHz); osmocom opens a V4 but hears nothing on HF.
   Bias tee is forced off on open: 4.5 V would go into a rig's IF output. */
class RtlSdrDevice : public SdrDevice
{
    Q_OBJECT

public:
    explicit RtlSdrDevice(QObject *parent = nullptr);
    ~RtlSdrDevice() override;

    void setLibraryPath(const QString &path) override;
    QString libraryPath() const override;
    QStringList deviceNames() override;

    bool open(int index) override;
    void close() override;
    bool isOpen() const override { return dev != nullptr; }
    QString description() const override { return about; }

    bool setSampleRate(int hz) override;
    int sampleRate() const override { return rate; }
    bool setCenterFrequency(double hz) override;
    double centerFrequency() const override { return center; }
    bool setGain(double db) override;
    QVector<double> gains() const override { return gainSteps; }
    bool setFrequencyCorrection(int ppm) override;

    /* 0 off, 1 I branch, 2 Q branch; HF on V3 and older. Keep 0 on a V4. */
    bool setDirectSampling(int mode) override;
    /* Never into a rig. */
    bool setBiasTee(bool on) override;

    bool start() override;
    void stop() override;
    bool isRunning() const override { return running; }

private:
    struct rtlsdr_dev;
    using ReadCallback = void (*)(unsigned char *, quint32, void *);

    bool load();
    static void received(unsigned char *buffer, quint32 length, void *self);
    void readLoop();

    QLibrary library;
    QString chosenPath;

    quint32 (*getDeviceCount)(void) = nullptr;
    const char *(*getDeviceName)(quint32) = nullptr;
    int (*getDeviceUsbStrings)(quint32, char *, char *, char *) = nullptr;
    int (*openDevice)(rtlsdr_dev **, quint32) = nullptr;
    int (*closeDevice)(rtlsdr_dev *) = nullptr;
    int (*setCenterFreq)(rtlsdr_dev *, quint32) = nullptr;
    int (*setFreqCorrection)(rtlsdr_dev *, int) = nullptr;
    int (*getTunerType)(rtlsdr_dev *) = nullptr;
    int (*getTunerGains)(rtlsdr_dev *, int *) = nullptr;
    int (*setTunerGain)(rtlsdr_dev *, int) = nullptr;
    int (*setTunerGainMode)(rtlsdr_dev *, int) = nullptr;
    int (*setRate)(rtlsdr_dev *, quint32) = nullptr;
    quint32 (*getRate)(rtlsdr_dev *) = nullptr;
    int (*setAgcMode)(rtlsdr_dev *, int) = nullptr;
    int (*setDirectSamplingMode)(rtlsdr_dev *, int) = nullptr;
    int (*setBiasTeePower)(rtlsdr_dev *, int) = nullptr;
    int (*resetBuffer)(rtlsdr_dev *) = nullptr;
    int (*readAsync)(rtlsdr_dev *, ReadCallback, void *, quint32, quint32) = nullptr;
    int (*cancelAsync)(rtlsdr_dev *) = nullptr;

    rtlsdr_dev *dev = nullptr;
    QString about;
    QVector<double> gainSteps;
    int rate = 0;
    double center = 0.0;

    std::thread reader;
    std::atomic<bool> running {false};
    std::atomic<bool> stopping {false};
    /* Byte -> float lookup. */
    float level[256];
};

#endif // QLOG_SDR_RTLSDRDEVICE_H
