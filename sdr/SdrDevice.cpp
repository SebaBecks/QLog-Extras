#include "SdrDevice.h"
#include "RtlSdrDevice.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.sdr.sdrdevice");

QStringList SdrDevice::driverKeys()
{
    FCT_IDENTIFICATION;

    return { QStringLiteral("rtlsdr") };
}

QString SdrDevice::driverName(const QString &key)
{
    FCT_IDENTIFICATION;

    if ( key == QLatin1String("rtlsdr") )
        return tr("RTL-SDR (librtlsdr)");
    return QString();
}

SdrDevice *SdrDevice::create(const QString &key, QObject *parent)
{
    FCT_IDENTIFICATION;

    if ( key == QLatin1String("rtlsdr") )
        return new RtlSdrDevice(parent);
    return nullptr;
}
