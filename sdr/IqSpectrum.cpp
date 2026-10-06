#include <cmath>

#include <QtMath>

#include "IqSpectrum.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.sdr.iqspectrum");

/* log10 floor, -200 dB, far below any ADC. */
#define POWER_FLOOR      1e-20

IqSpectrum::IqSpectrum(QObject *parent) :
    QObject(parent)
{
    FCT_IDENTIFICATION;

    prepare();
}

void IqSpectrum::setSampleRate(int hz)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << hz;

    rate = hz;
    reset();
}

void IqSpectrum::setSize(int count)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << count;

    int powerOfTwo = 256;
    while ( powerOfTwo < count && powerOfTwo < ( 1 << 20 ) )
        powerOfTwo <<= 1;

    if ( powerOfTwo == bins )
        return;
    bins = powerOfTwo;
    prepare();
}

void IqSpectrum::setAverage(int blocks)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << blocks;

    average = qMax(1, blocks);
    reset();
}

void IqSpectrum::setRate(double spectraPerSecond)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << spectraPerSecond;

    perSecond = qMax(0.1, spectraPerSecond);
}

void IqSpectrum::prepare()
{
    FCT_IDENTIFICATION;

    /* Blackman-Harris: -92 dB sidelobes, strong stations do not mask weak neighbours. */
    window.resize(bins);
    double sum = 0.0;
    for ( int i = 0; i < bins; i++ )
    {
        const double x = 2.0 * M_PI * i / ( bins - 1 );
        window[i] = float(0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x));
        sum += window[i];
    }
    /* 0 dB = full-scale complex tone (window sum squared in its bin). */
    scale = float(1.0 / ( sum * sum ));

    twiddle.resize(bins / 2);
    for ( int i = 0; i < bins / 2; i++ )
        twiddle[i] = std::polar(1.0f, float(-2.0 * M_PI * i / bins));

    int levels = 0;
    while ( ( 1 << levels ) < bins )
        levels++;
    reversed.resize(bins);
    for ( int i = 0; i < bins; i++ )
    {
        int r = 0;
        for ( int b = 0; b < levels; b++ )
            if ( i & ( 1 << b ) )
                r |= 1 << ( levels - 1 - b );
        reversed[i] = r;
    }

    reset();
}

void IqSpectrum::reset()
{
    FCT_IDENTIFICATION;

    block.resize(bins);
    power.fill(0.0, bins);
    filled = 0;
    blocksDone = 0;
    skip = 0;
}

void IqSpectrum::transform(std::complex<float> *data) const
{
    FCT_IDENTIFICATION;

    for ( int i = 0; i < bins; i++ )
        if ( i < reversed[i] )
            std::swap(data[i], data[reversed[i]]);

    for ( int length = 2; length <= bins; length <<= 1 )
    {
        const int half = length / 2;
        const int step = bins / length;

        for ( int start = 0; start < bins; start += length )
        {
            for ( int k = 0; k < half; k++ )
            {
                const std::complex<float> t = twiddle[k * step] * data[start + k + half];
                data[start + k + half] = data[start + k] - t;
                data[start + k] += t;
            }
        }
    }
}

void IqSpectrum::process(const SdrDevice::Samples &samples)
{
    FCT_IDENTIFICATION;

    const std::complex<float> *in = samples.constData();
    qint64 left = samples.size();

    while ( left > 0 )
    {
        if ( skip > 0 )
        {
            const qint64 dropped = qMin(skip, left);
            skip -= dropped;
            in += dropped;
            left -= dropped;
            continue;
        }

        const int taken = int(qMin<qint64>(bins - filled, left));
        for ( int i = 0; i < taken; i++ )
            block[filled + i] = in[i] * window[filled + i];
        filled += taken;
        in += taken;
        left -= taken;

        if ( filled < bins )
            break;

        transform(block.data());
        for ( int i = 0; i < bins; i++ )
            power[i] += std::norm(block[i]);
        filled = 0;

        if ( ++blocksDone < average )
            continue;

        /* FFT puts DC first, negative frequencies in the upper half; centre it. */
        QVector<float> decibels(bins);
        const int half = bins / 2;
        const double norm = double(scale) / average;
        for ( int i = 0; i < bins; i++ )
        {
            const double p = power[( i + half ) % bins] * norm;
            decibels[i] = float(10.0 * std::log10(qMax(p, POWER_FLOOR)));
        }
        power.fill(0.0);
        blocksDone = 0;

        if ( rate > 0 )
            skip = qMax<qint64>(0, qint64(rate / perSecond) - qint64(average) * bins);

        emit spectrumReady(decibels);
    }
}
