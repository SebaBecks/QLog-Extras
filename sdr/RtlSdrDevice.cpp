#include <cmath>

#include "RtlSdrDevice.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.sdr.rtlsdrdevice");

/* librtlsdr defaults: 15 x 256 kB; one buffer is ~55 ms at 2.4 MS/s. */
#define BUFFER_COUNT     15
#define BUFFER_BYTES     (16 * 32 * 512)
/* ADC zero is half a step above 127. */
#define ADC_ZERO         127.4f

static const char *tunerName(int type)
{
    FCT_IDENTIFICATION;

    switch ( type )
    {
    case 1: return "E4000";
    case 2: return "FC0012";
    case 3: return "FC0013";
    case 4: return "FC2580";
    case 5: return "R820T";
    case 6: return "R828D";
    default: return "unknown tuner";
    }
}

/* Template, not a generic lambda: Qt 5 builds are C++11 (QLog.pro). */
template<typename Function>
static void resolveSymbol(QLibrary &library, Function &function, const char *name, bool &complete)
{
    FCT_IDENTIFICATION;

    function = reinterpret_cast<Function>(library.resolve(name));

    if ( !function )
    {
        qCWarning(runtime) << "missing in the RTL-SDR library:" << name;
        complete = false;
    }
}

RtlSdrDevice::RtlSdrDevice(QObject *parent) :
    SdrDevice(parent)
{
    FCT_IDENTIFICATION;

    qRegisterMetaType<SdrDevice::Samples>();

    for ( int i = 0; i < 256; i++ )
        level[i] = ( i - ADC_ZERO ) / 128.0f;
}

RtlSdrDevice::~RtlSdrDevice()
{
    FCT_IDENTIFICATION;

    close();
}

void RtlSdrDevice::setLibraryPath(const QString &path)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << path;

    if ( path == chosenPath )
        return;

    close();
    if ( library.isLoaded() )
        library.unload();
    chosenPath = path;
}

QString RtlSdrDevice::libraryPath() const
{
    FCT_IDENTIFICATION;

    if ( library.isLoaded() )
        return library.fileName();
    return chosenPath.isEmpty() ? QStringLiteral("rtlsdr") : chosenPath;
}

bool RtlSdrDevice::load()
{
    FCT_IDENTIFICATION;

    if ( library.isLoaded() )
        return true;

    /* Windows searches QLog's folder first (deployed DLL). Linux distros ship
       librtlsdr.so.0, manual installs often lack the version suffix. */
    bool loaded;

    if ( chosenPath.isEmpty() )
    {
        library.setFileNameAndVersion(QStringLiteral("rtlsdr"), 0);
        loaded = library.load();
        if ( !loaded )
        {
            library.setFileName(QStringLiteral("rtlsdr"));
            loaded = library.load();
        }
    }
    else
    {
        library.setFileName(chosenPath);
        loaded = library.load();
    }

    if ( !loaded )
        return fail(tr("The RTL-SDR library could not be loaded: %1").arg(library.errorString()));

    bool complete = true;

    resolveSymbol(library, getDeviceCount, "rtlsdr_get_device_count", complete);
    resolveSymbol(library, getDeviceName, "rtlsdr_get_device_name", complete);
    resolveSymbol(library, getDeviceUsbStrings, "rtlsdr_get_device_usb_strings", complete);
    resolveSymbol(library, openDevice, "rtlsdr_open", complete);
    resolveSymbol(library, closeDevice, "rtlsdr_close", complete);
    resolveSymbol(library, setCenterFreq, "rtlsdr_set_center_freq", complete);
    resolveSymbol(library, setFreqCorrection, "rtlsdr_set_freq_correction", complete);
    resolveSymbol(library, getTunerType, "rtlsdr_get_tuner_type", complete);
    resolveSymbol(library, getTunerGains, "rtlsdr_get_tuner_gains", complete);
    resolveSymbol(library, setTunerGain, "rtlsdr_set_tuner_gain", complete);
    resolveSymbol(library, setTunerGainMode, "rtlsdr_set_tuner_gain_mode", complete);
    resolveSymbol(library, setRate, "rtlsdr_set_sample_rate", complete);
    resolveSymbol(library, getRate, "rtlsdr_get_sample_rate", complete);
    resolveSymbol(library, setAgcMode, "rtlsdr_set_agc_mode", complete);
    resolveSymbol(library, setDirectSamplingMode, "rtlsdr_set_direct_sampling", complete);
    resolveSymbol(library, resetBuffer, "rtlsdr_reset_buffer", complete);
    resolveSymbol(library, readAsync, "rtlsdr_read_async", complete);
    resolveSymbol(library, cancelAsync, "rtlsdr_cancel_async", complete);

    /* Optional, newer libraries only; without it the tee cannot be turned off. */
    setBiasTeePower = reinterpret_cast<decltype(setBiasTeePower)>(library.resolve("rtlsdr_set_bias_tee"));

    if ( !complete )
    {
        library.unload();
        return fail(tr("%1 is not a usable RTL-SDR library").arg(library.fileName()));
    }

    qCDebug(runtime) << "loaded" << library.fileName();
    return true;
}

QStringList RtlSdrDevice::deviceNames()
{
    FCT_IDENTIFICATION;

    QStringList names;

    if ( !load() )
        return names;

    const quint32 count = getDeviceCount();

    for ( quint32 i = 0; i < count; i++ )
    {
        char vendor[256] = {}, product[256] = {}, serial[256] = {};
        QString name = QString::fromUtf8(getDeviceName(i));

        if ( getDeviceUsbStrings(i, vendor, product, serial) == 0 )
        {
            const QString usbName = QString::fromUtf8(product).trimmed();
            if ( !usbName.isEmpty() )
                name = usbName;
            if ( serial[0] )
                name += QStringLiteral(" (SN %1)").arg(QString::fromUtf8(serial));
        }
        names << name;
    }
    return names;
}

bool RtlSdrDevice::open(int index)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << index;

    close();

    if ( !load() )
        return false;

    const quint32 count = getDeviceCount();

    if ( index < 0 || quint32(index) >= count )
        return fail(count ? tr("There is no RTL-SDR number %1").arg(index + 1)
                          : tr("No RTL-SDR is plugged in"));

    if ( openDevice(&dev, quint32(index)) != 0 || !dev )
    {
        dev = nullptr;
        return fail(tr("The RTL-SDR could not be opened. Another program may be "
                       "using it, or it has no WinUSB driver (Zadig)."));
    }

    if ( setBiasTeePower )
        setBiasTeePower(dev, 0);
    setAgcMode(dev, 0);
    setTunerGainMode(dev, 1);

    gainSteps.clear();
    const int steps = getTunerGains(dev, nullptr);
    if ( steps > 0 )
    {
        QVector<int> tenths(steps);
        getTunerGains(dev, tenths.data());
        for ( int g : tenths )
            gainSteps << g / 10.0;
    }

    const QStringList names = deviceNames();
    about = QStringLiteral("%1, %2").arg(names.value(index), QString::fromLatin1(tunerName(getTunerType(dev))));

    qCDebug(runtime) << "opened" << about << "gains" << gainSteps;
    return true;
}

void RtlSdrDevice::close()
{
    FCT_IDENTIFICATION;

    stop();

    if ( dev )
    {
        closeDevice(dev);
        dev = nullptr;
    }
    about.clear();
    gainSteps.clear();
    rate = 0;
    center = 0.0;
}

bool RtlSdrDevice::setSampleRate(int hz)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << hz;

    if ( !dev )
        return fail(tr("The RTL-SDR is not open"));

    if ( setRate(dev, quint32(hz)) != 0 )
        return fail(tr("The RTL-SDR refused a sample rate of %1 Hz").arg(hz));

    /* Actual rate differs slightly from the requested one. */
    rate = int(getRate(dev));
    return true;
}

bool RtlSdrDevice::setCenterFrequency(double hz)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << hz;

    if ( !dev )
        return fail(tr("The RTL-SDR is not open"));

    if ( hz <= 0 || hz > 4.0e9 || setCenterFreq(dev, quint32(std::llround(hz))) != 0 )
        return fail(tr("The RTL-SDR cannot tune to %1 Hz").arg(hz, 0, 'f', 0));

    center = std::llround(hz);
    return true;
}

bool RtlSdrDevice::setGain(double db)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << db;

    if ( !dev )
        return fail(tr("The RTL-SDR is not open"));

    if ( db < 0 || gainSteps.isEmpty() )
        return setTunerGainMode(dev, 0) == 0 ? true : fail(tr("The RTL-SDR has no automatic gain"));

    /* Tuner supports discrete steps only. */
    double nearest = gainSteps.first();
    for ( double step : static_cast<const QVector<double> &>(gainSteps) )
        if ( std::fabs(step - db) < std::fabs(nearest - db) )
            nearest = step;

    if ( setTunerGainMode(dev, 1) != 0 || setTunerGain(dev, int(std::lround(nearest * 10))) != 0 )
        return fail(tr("The RTL-SDR refused a gain of %1 dB").arg(nearest));
    return true;
}

bool RtlSdrDevice::setFrequencyCorrection(int ppm)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << ppm;

    if ( !dev )
        return fail(tr("The RTL-SDR is not open"));

    /* -2 = value already set. */
    const int result = setFreqCorrection(dev, ppm);
    return ( result == 0 || result == -2 ) ? true : fail(tr("The RTL-SDR refused a correction of %1 ppm").arg(ppm));
}

bool RtlSdrDevice::setDirectSampling(int mode)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << mode;

    if ( !dev )
        return fail(tr("The RTL-SDR is not open"));

    return setDirectSamplingMode(dev, mode) == 0 ? true : fail(tr("The RTL-SDR has no direct sampling"));
}

bool RtlSdrDevice::setBiasTee(bool on)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << on;

    if ( !dev )
        return fail(tr("The RTL-SDR is not open"));
    if ( !setBiasTeePower )
        return fail(tr("This RTL-SDR library cannot switch the bias tee"));

    return setBiasTeePower(dev, on ? 1 : 0) == 0 ? true : fail(tr("The RTL-SDR refused to switch the bias tee"));
}

bool RtlSdrDevice::start()
{
    FCT_IDENTIFICATION;

    if ( running )
        return true;
    if ( !dev )
        return fail(tr("The RTL-SDR is not open"));
    if ( rate <= 0 )
        return fail(tr("No sample rate is set"));

    resetBuffer(dev);
    stopping = false;
    running = true;
    reader = std::thread(&RtlSdrDevice::readLoop, this);
    return true;
}

void RtlSdrDevice::stop()
{
    FCT_IDENTIFICATION;

    if ( !reader.joinable() )
        return;

    stopping = true;
    cancelAsync(dev);
    reader.join();
    running = false;
}

void RtlSdrDevice::readLoop()
{
    FCT_IDENTIFICATION;

    /* Blocks until cancelled or unplugged. */
    const int result = readAsync(dev, &RtlSdrDevice::received, this, BUFFER_COUNT, BUFFER_BYTES);

    running = false;
    if ( !stopping )
        emit failed(tr("The RTL-SDR stopped sending (%1)").arg(result));
}

void RtlSdrDevice::received(unsigned char *buffer, quint32 length, void *self)
{
    FCT_IDENTIFICATION;

    RtlSdrDevice *that = static_cast<RtlSdrDevice *>(self);

    if ( that->stopping )
        return;

    Samples samples(int(length / 2));
    std::complex<float> *out = samples.data();

    for ( quint32 i = 0; i + 1 < length; i += 2 )
        *out++ = std::complex<float>(that->level[buffer[i]], that->level[buffer[i + 1]]);

    emit that->samplesReady(samples);
}
