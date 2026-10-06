#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include "OmniRigMeters.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.rig.omnirigmeters");

namespace
{

struct CalPoint
{
    int raw;
    double value;
};

/* Calibration tables copied from Hamlib (source define named above each).
   One rig's table stands in for its family; S cal/S9+ absorb differences. */

// FTDX101D_STR_CAL: SM0, 0-255, against decibels relative to S9
const CalPoint YAESU_STRENGTH[] =
{
    {   0, -60 }, {  17, -54 }, {  25, -48 }, {  34, -42 },
    {  51, -36 }, {  68, -30 }, {  85, -24 }, { 102, -18 },
    { 119, -12 }, { 136,  -6 }, { 160,   0 }, { 255,  60 }
};

// FTDX101D_RFPOWER_METER_WATTS_CAL
const CalPoint FTDX101D_WATTS[] =
{
    {   0,   0 }, {  38,   5 }, {  94,  25 }, { 147,  50 }, { 176,  75 }, { 205, 100 }
};

// FTDX101MP_RFPOWER_METER_WATTS_CAL
const CalPoint FTDX101MP_WATTS[] =
{
    {   0,   0 }, {  30,   5 }, {  69,  20 }, {  98,  40 }, { 119,  60 },
    { 139,  80 }, { 160, 100 }, { 173, 120 }, { 185, 140 }, { 198, 160 },
    { 210, 180 }, { 225, 200 }, { 255, 210 }
};

// yaesu_default_rfpower_meter_cal
const CalPoint YAESU_WATTS[] =
{
    {   0,   0 }, { 148,  50 }, { 255, 100 }
};

// FTDX101D_SWR_CAL
const CalPoint FTDX101_SWR[] =
{
    {   0, 1.0 }, {  26, 1.2 }, {  52, 1.5 }, {  89, 2.0 },
    { 126, 3.0 }, { 173, 4.0 }, { 236, 5.0 }, { 255, 25.0 }
};

// yaesu_default_swr_cal
const CalPoint YAESU_SWR[] =
{
    {  12, 1.0 }, {  39, 1.35 }, {  65, 1.5 }, {  89, 2.0 }, { 242, 5.0 }
};

// FTDX101D_ALC_METER_CAL
const CalPoint FTDX101_ALC[] =
{
    {   0, 0.0 }, { 121, 1.0 }
};

// yaesu_default_alc_cal
const CalPoint YAESU_ALC[] =
{
    {   0, 0.0 }, {  64, 1.0 }
};

// TS590_STR_CAL; TS2000_STR_CAL differs only in where S0 sits
const CalPoint KENWOOD_STRENGTH[] =
{
    {  0, -60 }, {  3, -48 }, {  6, -36 }, {  9, -24 }, { 12, -12 },
    { 15,   0 }, { 20,  20 }, { 25,  40 }, { 30,  60 }
};

// the power_meter table in ts590_get_level: SM0 while transmitting
const CalPoint KENWOOD_WATTS[] =
{
    {  0,   0 }, {  3,   5 }, {  6,  10 }, {  8,  15 }, { 12,  25 }, { 17,  50 }, { 30, 100 }
};

// TS590_SWR_CAL
const CalPoint KENWOOD_SWR[] =
{
    {  0, 1.0 }, {  6, 1.5 }, { 12, 2.0 }, { 18, 3.0 }, { 30, 10.0 }
};

// IC7300_STR_CAL, which the IC-705 and IC-9700 share
const CalPoint ICOM_STRENGTH[] =
{
    {   0, -54 }, {  10, -48 }, {  30, -36 }, {  60, -24 },
    {  90, -12 }, { 120,   0 }, { 241,  64 }
};

// IC7300_RFPOWER_METER_CAL, in watts
const CalPoint ICOM_WATTS[] =
{
    {   0,   0 }, {  21,   5 }, {  43,  10 }, {  65,  15 }, {  83,  20 },
    {  95,  25 }, { 105,  30 }, { 114,  35 }, { 124,  40 }, { 143,  50 },
    { 183,  75 }, { 213, 100 }, { 255, 120 }
};

// IC705_RFPOWER_METER_CAL, in watts
const CalPoint IC705_WATTS[] =
{
    {   0, 0.0 }, {  21, 0.5 }, {  43, 1.0 }, {  65, 1.5 }, {  83, 2.0 },
    {  95, 2.5 }, { 105, 3.0 }, { 114, 3.5 }, { 124, 4.0 }, { 143, 5.0 },
    { 183, 7.5 }, { 213, 10.0 }, { 255, 12.0 }
};

// IC7300_SWR_CAL
const CalPoint ICOM_SWR[] =
{
    {   0, 1.0 }, {  48, 1.5 }, {  80, 2.0 }, { 120, 3.0 }, { 240, 6.0 }
};

// IC7300_ALC_CAL
const CalPoint ICOM_ALC[] =
{
    {   0, 0.0 }, { 120, 1.0 }
};

// straight lines between the points, as Hamlib's rig_raw2val does
template<size_t N>
double interpolate(const CalPoint (&table)[N], int raw)
{
    FCT_IDENTIFICATION;

    if ( raw <= table[0].raw )
        return table[0].value;

    for ( size_t i = 1; i < N; ++i )
    {
        if ( raw <= table[i].raw )
        {
            const CalPoint &low = table[i - 1];
            const CalPoint &high = table[i];
            return low.value + ( high.value - low.value ) * ( raw - low.raw ) / ( high.raw - low.raw );
        }
    }

    return table[N - 1].value;
}

// one key of one section of a rig file, hex commands with their dots removed
QString sectionValue(const QString &path, const QString &section, const QString &key)
{
    FCT_IDENTIFICATION;

    QFile file(path);

    if ( !file.open(QIODevice::ReadOnly | QIODevice::Text) )
        return QString();

    QTextStream in(&file);
    const QString header = QChar('[') + section + QChar(']');
    bool inSection = false;

    while ( !in.atEnd() )
    {
        const QString line = in.readLine().trimmed();

        if ( line.startsWith(QChar('[')) )
        {
            inSection = ( line.compare(header, Qt::CaseInsensitive) == 0 );
            continue;
        }

        if ( !inSection )
            continue;

        const int equals = line.indexOf(QChar('='));

        if ( equals > 0 && line.left(equals).trimmed().compare(key, Qt::CaseInsensitive) == 0 )
            return line.mid(equals + 1).trimmed().remove(QChar('.')).toUpper();
    }

    return QString();
}

int fromBcd(unsigned char byte)
{
    FCT_IDENTIFICATION;

    return ( byte >> 4 ) * 10 + ( byte & 0x0F );
}

const char ICOM_STRENGTH_SUB = 0x02;
const char ICOM_POWER_SUB = 0x11;
const char ICOM_SWR_SUB = 0x12;
const char ICOM_ALC_SUB = 0x13;

OmniRigMeters::Request icomRequest(const OmniRigMeters::RigInfo &rig, char sub)
{
    FCT_IDENTIFICATION;

    OmniRigMeters::Request request;
    request.command = QByteArray::fromHex("FEFE") + rig.civAddress
                    + QByteArray::fromHex("E015") + sub + QByteArray::fromHex("FD");
    // the answer is nine bytes, after the echo of the seven sent
    request.replyLength = ( rig.civEcho ? request.command.size() : 0 ) + 9;
    return request;
}

/* An Icom answers FE FE E0 <rig> 15 <sub> <two BCD bytes> FD, after the echo
   of the command when the rig echoes. Every meter runs from 0000 to 0255. */
bool icomReading(const QByteArray &reply, char address, char &sub, int &raw)
{
    FCT_IDENTIFICATION;

    const QByteArray head = QByteArray::fromHex("FEFEE0") + address + QByteArray::fromHex("15");
    const int at = reply.indexOf(head);
    const int data = at + head.size();

    if ( at < 0 || reply.size() < data + 4
         || static_cast<unsigned char>(reply.at(data + 3)) != 0xFD )
        return false;

    sub = reply.at(data);
    raw = fromBcd(static_cast<unsigned char>(reply.at(data + 1))) * 100
        + fromBcd(static_cast<unsigned char>(reply.at(data + 2)));
    return true;
}

OmniRigMeters::Request textRequest(const char *command, int replyLength = 0)
{
    FCT_IDENTIFICATION;

    OmniRigMeters::Request request;
    request.command = QByteArray(command);
    request.replyLength = replyLength;

    if ( replyLength == 0 )
        request.replyEnd = QByteArrayLiteral(";");

    return request;
}

}

OmniRigMeters::RigInfo OmniRigMeters::describe(const QString &rigType, const QString &rigFile,
                                               double maxWatts)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << rigType << rigFile << maxWatts;

    /* Text-protocol Yaesus: FTDX*, FT-450/710/891/950/991/2000/9000 (9000 has
       different RM numbering, no TX meters). Kenwood: SM0 0-30 on TS-480/590/
       2000 only; RM with all three TX meters only on TS-590. */
    static const QRegularExpression yaesu(QStringLiteral("^FT(DX|-?(450|710|891|950|991|2000|2K|9000))"),
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression yaesu9000(QStringLiteral("^FT(DX)?-?9000"),
                                              QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kenwood(QStringLiteral("^TS-"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kenwoodSm0(QStringLiteral("^TS-(480|590|2000)"),
                                               QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression icom(QStringLiteral("^IC-"),
                                         QRegularExpression::CaseInsensitiveOption);

    RigInfo rig;
    rig.maxWatts = maxWatts;

    if ( yaesu.match(rigType).hasMatch() )
    {
        rig.family = YaesuFamily;
        rig.strength = true;
        rig.transmitMeters = !yaesu9000.match(rigType).hasMatch();
        rig.ftdx101 = rigType.startsWith(QStringLiteral("FTDX101"), Qt::CaseInsensitive);
    }
    else if ( kenwood.match(rigType).hasMatch() )
    {
        rig.family = KenwoodFamily;
        rig.strength = kenwoodSm0.match(rigType).hasMatch();
        rig.transmitMeters = rigType.startsWith(QStringLiteral("TS-590"), Qt::CaseInsensitive);
    }
    else if ( icom.match(rigType).hasMatch() )
    {
        rig.family = IcomFamily;
        rig.ic705 = rigType.startsWith(QStringLiteral("IC-705"), Qt::CaseInsensitive);

        /* CI-V address from STATUS1 Command (FE FE <rig> E0); the rig echoes
           if Validate starts with the command itself. */
        const QString command = sectionValue(rigFile, QStringLiteral("STATUS1"), QStringLiteral("Command"));
        QString validate = sectionValue(rigFile, QStringLiteral("STATUS1"), QStringLiteral("Validate"));

        // older rig files use mask|value, e.g. FFFF...|FEFE66E003FD...
        const int bar = validate.indexOf(QChar('|'));

        if ( bar >= 0 )
            validate = validate.mid(bar + 1);

        static const QRegularExpression civ(QStringLiteral("^FEFE([0-9A-F]{2})E0"));
        const QRegularExpressionMatch match = civ.match(command);

        if ( match.hasMatch() )
        {
            rig.civAddress = static_cast<char>(match.captured(1).toInt(nullptr, 16));
            rig.civEcho = validate.startsWith(command);
            rig.strength = true;
            rig.transmitMeters = true;
        }
    }

    qCDebug(runtime) << rigType << "family" << rig.family << "strength" << rig.strength
                     << "transmit" << rig.transmitMeters
                     << "CI-V" << QString::number(static_cast<unsigned char>(rig.civAddress), 16)
                     << "echo" << rig.civEcho;
    return rig;
}

OmniRigMeters::Request OmniRigMeters::strengthRequest(const RigInfo &rig)
{
    FCT_IDENTIFICATION;

    if ( !rig.strength )
        return Request();

    switch ( rig.family )
    {
    case YaesuFamily:
    case KenwoodFamily:
        // main receiver; Yaesu answers with three digits, Kenwood with four
        return textRequest("SM0;");

    case IcomFamily:
        return icomRequest(rig, ICOM_STRENGTH_SUB);

    default:
        return Request();
    }
}

QList<OmniRigMeters::Request> OmniRigMeters::transmitRequests(const RigInfo &rig)
{
    FCT_IDENTIFICATION;

    QList<Request> requests;

    if ( !rig.transmitMeters )
        return requests;

    switch ( rig.family )
    {
    case YaesuFamily:
        // ALC matters as much as power: it is what says the drive is right
        requests << textRequest("RM5;") << textRequest("RM6;") << textRequest("RM4;");
        break;

    case KenwoodFamily:
        // SM0 is the power meter while transmitting; RM answers RM1..;RM2..;RM3..;
        requests << textRequest("SM0;") << textRequest("RM;", 24);
        break;

    case IcomFamily:
        requests << icomRequest(rig, ICOM_POWER_SUB) << icomRequest(rig, ICOM_SWR_SUB)
                 << icomRequest(rig, ICOM_ALC_SUB);
        break;

    default:
        break;
    }

    return requests;
}

QList<OmniRigMeters::Reading> OmniRigMeters::parse(const RigInfo &rig, const QByteArray &reply,
                                                   bool transmitting)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << rig.family << reply.toHex() << transmitting;

    QList<Reading> readings;
    const QString text = QString::fromLatin1(reply);

    switch ( rig.family )
    {
    case YaesuFamily:
    {
        static const QRegularExpression strength(QStringLiteral("^SM0(\\d{3});"));
        // the FTDX101 adds three more digits that say nothing here
        static const QRegularExpression meter(QStringLiteral("^RM([456])(\\d{3})"));

        QRegularExpressionMatch match = strength.match(text);

        if ( match.hasMatch() )
        {
            readings.append({ Strength, interpolate(YAESU_STRENGTH, match.captured(1).toInt()) });
            break;
        }

        match = meter.match(text);

        if ( !match.hasMatch() )
            break;

        const int raw = match.captured(2).toInt();

        switch ( match.captured(1).at(0).toLatin1() )
        {
        case '5':
            readings.append({ Power, !rig.ftdx101 ? interpolate(YAESU_WATTS, raw)
                                     : rig.maxWatts > 100.0 ? interpolate(FTDX101MP_WATTS, raw)
                                     : interpolate(FTDX101D_WATTS, raw) });
            break;
        case '6':
            readings.append({ Swr, rig.ftdx101 ? interpolate(FTDX101_SWR, raw)
                                               : interpolate(YAESU_SWR, raw) });
            break;
        case '4':
            readings.append({ Alc, rig.ftdx101 ? interpolate(FTDX101_ALC, raw)
                                               : interpolate(YAESU_ALC, raw) });
            break;
        }
        break;
    }

    case KenwoodFamily:
    {
        static const QRegularExpression strength(QStringLiteral("^SM0(\\d{4});"));
        static const QRegularExpression meters(QStringLiteral("^RM1(\\d{4});RM2(\\d{4});RM3(\\d{4});"));

        QRegularExpressionMatch match = strength.match(text);

        if ( match.hasMatch() )
        {
            const int raw = match.captured(1).toInt();

            if ( transmitting )
                readings.append({ Power, interpolate(KENWOOD_WATTS, raw) });
            else
                readings.append({ Strength, interpolate(KENWOOD_STRENGTH, raw) });
            break;
        }

        match = meters.match(text);

        if ( match.hasMatch() )
        {
            readings.append({ Swr, interpolate(KENWOOD_SWR, match.captured(1).toInt()) });
            // Hamlib divides by six, putting the 0-30 scale near other rigs' ALC
            readings.append({ Alc, match.captured(3).toInt() / 6.0 });
        }
        break;
    }

    case IcomFamily:
    {
        char sub = 0;
        int raw = 0;

        if ( !icomReading(reply, rig.civAddress, sub, raw) )
            break;

        if ( sub == ICOM_STRENGTH_SUB )
            readings.append({ Strength, interpolate(ICOM_STRENGTH, raw) });
        else if ( sub == ICOM_POWER_SUB )
            readings.append({ Power, rig.ic705 ? interpolate(IC705_WATTS, raw)
                                               : interpolate(ICOM_WATTS, raw) });
        else if ( sub == ICOM_SWR_SUB )
            readings.append({ Swr, interpolate(ICOM_SWR, raw) });
        else if ( sub == ICOM_ALC_SUB )
            readings.append({ Alc, interpolate(ICOM_ALC, raw) });
        break;
    }

    default:
        break;
    }

    return readings;
}
