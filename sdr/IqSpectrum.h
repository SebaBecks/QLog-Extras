#ifndef QLOG_SDR_IQSPECTRUM_H
#define QLOG_SDR_IQSPECTRUM_H

#include <QObject>
#include <QVector>
#include <complex>

#include "SdrDevice.h"

/* I/Q samples -> centred power spectra in dB relative to full scale.
   A few blocks are averaged per spectrum; samples in between are skipped. */
class IqSpectrum : public QObject
{
    Q_OBJECT

public:
    explicit IqSpectrum(QObject *parent = nullptr);

    void setSampleRate(int hz);
    /* Power of two. */
    void setSize(int bins);
    void setAverage(int blocks);
    /* Upper limit, spectra per second. */
    void setRate(double perSecond);

    int size() const { return bins; }
    int sampleRate() const { return rate; }
    double binWidth() const { return rate > 0 ? double(rate) / bins : 0.0; }
    /* Hz from the centre frequency. */
    double offsetForBin(int index) const { return ( index - bins / 2 ) * binWidth(); }

public slots:
    void process(const SdrDevice::Samples &samples);
    void reset();

signals:
    void spectrumReady(const QVector<float> &decibels);

private:
    void prepare();
    void transform(std::complex<float> *data) const;

    int rate = 0;
    int bins = 65536;
    int average = 4;
    double perSecond = 10.0;

    QVector<float> window;
    QVector<std::complex<float>> twiddle;
    QVector<int> reversed;
    float scale = 1.0f;

    QVector<std::complex<float>> block;
    QVector<double> power;
    int filled = 0;
    int blocksDone = 0;
    /* Samples to drop before the next spectrum. */
    qint64 skip = 0;
};

#endif // QLOG_SDR_IQSPECTRUM_H
