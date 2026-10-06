#include <QRegularExpression>

#include "OmniRigControls.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.rig.omnirigcontrols");

using OmniRigMeters::Request;
using OmniRigMeters::RigInfo;

namespace
{

// Hamlib's rig_agc_level_e, which the panel shows
const int AGC_OFF = 0;
const int AGC_FAST = 2;
const int AGC_SLOW = 3;
const int AGC_MEDIUM = 5;
const int AGC_AUTO = 6;

Request textCommand(const QString &command, bool answered)
{
    FCT_IDENTIFICATION;

    Request request;
    request.command = command.toLatin1();

    // a setting is not answered, a reading is answered up to the semicolon
    if ( answered )
        request.replyEnd = QByteArrayLiteral(";");

    return request;
}

/* FE FE <rig> E0 <body> FD. The rig answers with answerBytes, a setting with
   FB, after the echo of the command when it echoes. */
Request icomCommand(const RigInfo &rig, const QByteArray &body, int answerBytes)
{
    FCT_IDENTIFICATION;

    Request request;
    request.command = QByteArray::fromHex("FEFE") + rig.civAddress + QByteArray::fromHex("E0")
                    + body + QByteArray::fromHex("FD");
    request.replyLength = ( rig.civEcho ? request.command.size() : 0 ) + answerBytes;
    return request;
}

// the CI-V command and subcommand of each control
QByteArray icomCode(OmniRigControls::Control control)
{
    FCT_IDENTIFICATION;

    switch ( control )
    {
    case OmniRigControls::Preamp:         return QByteArray::fromHex("1602");
    case OmniRigControls::Attenuator:     return QByteArray::fromHex("11");
    case OmniRigControls::Agc:            return QByteArray::fromHex("1612");
    case OmniRigControls::NoiseBlanker:   return QByteArray::fromHex("1622");
    case OmniRigControls::NoiseReduction: return QByteArray::fromHex("1640");
    }

    return QByteArray();
}

char toBcd(int value)
{
    FCT_IDENTIFICATION;

    return static_cast<char>(( ( value / 10 ) << 4 ) | ( value % 10 ));
}

int fromBcd(unsigned char byte)
{
    FCT_IDENTIFICATION;

    return ( byte >> 4 ) * 10 + ( byte & 0x0F );
}

int yaesuAgcToHamlib(int code)
{
    FCT_IDENTIFICATION;

    switch ( code )
    {
    case 0:  return AGC_OFF;
    case 1:  return AGC_FAST;
    case 2:  return AGC_MEDIUM;
    case 3:  return AGC_SLOW;
    default: return AGC_AUTO;    // 4, and 5 and 6 when auto has settled on a speed
    }
}

int hamlibAgcToYaesu(int mode)
{
    FCT_IDENTIFICATION;

    switch ( mode )
    {
    case AGC_FAST:   return 1;
    case AGC_MEDIUM: return 2;
    case AGC_SLOW:   return 3;
    case AGC_AUTO:   return 4;
    default:         return 0;
    }
}

int icomAgcToHamlib(int code)
{
    FCT_IDENTIFICATION;

    switch ( code )
    {
    case 1:  return AGC_FAST;
    case 2:  return AGC_MEDIUM;
    case 3:  return AGC_SLOW;
    default: return AGC_OFF;
    }
}

int hamlibAgcToIcom(int mode)
{
    FCT_IDENTIFICATION;

    switch ( mode )
    {
    case AGC_MEDIUM: return 2;
    case AGC_SLOW:   return 3;
    default:         return 1;
    }
}

}

bool OmniRigControls::supported(const RigInfo &rig)
{
    FCT_IDENTIFICATION;

    switch ( rig.family )
    {
    case OmniRigMeters::YaesuFamily:   return true;
    // only with the CI-V address found in the rig file
    case OmniRigMeters::IcomFamily:    return rig.strength;
    // the TS-590 is the one Kenwood whose commands are known here
    case OmniRigMeters::KenwoodFamily: return rig.transmitMeters;
    default:                           return false;
    }
}

Request OmniRigControls::readRequest(const RigInfo &rig, Control control)
{
    FCT_IDENTIFICATION;

    if ( !supported(rig) )
        return Request();

    switch ( rig.family )
    {
    case OmniRigMeters::YaesuFamily:
    {
        static const char *commands[] = { "PA0;", "RA0;", "GT0;", "NB0;", "NR0;" };
        return textCommand(QString::fromLatin1(commands[control]), true);
    }

    case OmniRigMeters::KenwoodFamily:
    {
        static const char *commands[] = { "PA;", "RA;", "GC;", "NB;", "NR;" };
        return textCommand(QString::fromLatin1(commands[control]), true);
    }

    case OmniRigMeters::IcomFamily:
        // FE FE E0 <rig> <code> <one byte> FD
        return icomCommand(rig, icomCode(control), 4 + icomCode(control).size() + 2);

    default:
        return Request();
    }
}

Request OmniRigControls::writeRequest(const RigInfo &rig, Control control, int value)
{
    FCT_IDENTIFICATION;

    if ( !supported(rig) )
        return Request();

    switch ( rig.family )
    {
    case OmniRigMeters::YaesuFamily:
        switch ( control )
        {
        // IPO, AMP1, AMP2 and 6, 12, 18 dB are the FTDX101's steps
        case Preamp:         return textCommand(QStringLiteral("PA0%1;").arg(value / 10), false);
        case Attenuator:     return textCommand(QStringLiteral("RA0%1;").arg(value / 6), false);
        case Agc:            return textCommand(QStringLiteral("GT0%1;").arg(hamlibAgcToYaesu(value)), false);
        case NoiseBlanker:   return textCommand(QStringLiteral("NB0%1;").arg(value ? 1 : 0), false);
        case NoiseReduction: return textCommand(QStringLiteral("NR0%1;").arg(value ? 1 : 0), false);
        }
        break;

    case OmniRigMeters::KenwoodFamily:
        switch ( control )
        {
        case Preamp:         return textCommand(QStringLiteral("PA%1;").arg(value ? 1 : 0), false);
        case Attenuator:     return textCommand(QStringLiteral("RA%1;").arg(value ? "01" : "00"), false);
        // the TS-590 numbers its AGC 1 slow, 2 fast
        case Agc:            return textCommand(QStringLiteral("GC%1;").arg(value == AGC_SLOW ? 1 : 2), false);
        case NoiseBlanker:   return textCommand(QStringLiteral("NB%1;").arg(value ? 1 : 0), false);
        case NoiseReduction: return textCommand(QStringLiteral("NR%1;").arg(value ? 1 : 0), false);
        }
        break;

    case OmniRigMeters::IcomFamily:
    {
        char data = 0;

        switch ( control )
        {
        case Preamp:         data = static_cast<char>(value / 10); break;
        // the attenuator is set in decibels, written as BCD
        case Attenuator:     data = toBcd(value); break;
        case Agc:            data = static_cast<char>(hamlibAgcToIcom(value)); break;
        case NoiseBlanker:
        case NoiseReduction: data = value ? 1 : 0; break;
        }

        // the rig answers FE FE E0 <rig> FB FD
        return icomCommand(rig, icomCode(control) + data, 6);
    }

    default:
        break;
    }

    return Request();
}

QList<int> OmniRigControls::steps(const RigInfo &rig, Control control)
{
    FCT_IDENTIFICATION;

    switch ( control )
    {
    case Preamp:
        return ( rig.family == OmniRigMeters::KenwoodFamily ) ? QList<int>{ 0, 12 }
                                                              : QList<int>{ 0, 10, 20 };
    case Attenuator:
        if ( rig.family == OmniRigMeters::KenwoodFamily )
            return { 0, 12 };
        if ( rig.family == OmniRigMeters::IcomFamily )
            return { 0, 20 };
        return { 0, 6, 12, 18 };

    case Agc:
        if ( rig.family == OmniRigMeters::KenwoodFamily )
            return { AGC_FAST, AGC_SLOW };
        if ( rig.family == OmniRigMeters::IcomFamily )
            return { AGC_FAST, AGC_MEDIUM, AGC_SLOW };
        return { AGC_FAST, AGC_MEDIUM, AGC_SLOW, AGC_AUTO };

    default:
        return { 0, 1 };
    }
}

QString OmniRigControls::preampName(const RigInfo &rig, int db)
{
    FCT_IDENTIFICATION;

    switch ( rig.family )
    {
    case OmniRigMeters::YaesuFamily:
        return db >= 20 ? QStringLiteral("AMP2") : db >= 10 ? QStringLiteral("AMP1") : QStringLiteral("IPO");
    case OmniRigMeters::IcomFamily:
        return db >= 20 ? QStringLiteral("P.AMP2") : db >= 10 ? QStringLiteral("P.AMP1") : QStringLiteral("off");
    default:
        return db > 0 ? QStringLiteral("on") : QStringLiteral("off");
    }
}

bool OmniRigControls::parse(const RigInfo &rig, const QByteArray &reply, State &state)
{
    FCT_IDENTIFICATION;

    if ( !supported(rig) )
        return false;

    const QString text = QString::fromLatin1(reply);

    switch ( rig.family )
    {
    case OmniRigMeters::YaesuFamily:
    {
        static const QRegularExpression answer(QStringLiteral("^(PA|RA|GT|NB|NR)0(\\d);"));
        const QRegularExpressionMatch match = answer.match(text);

        if ( !match.hasMatch() )
            return false;

        const QString name = match.captured(1);
        const int value = match.captured(2).toInt();

        if ( name == QLatin1String("PA") )      state = { Preamp, value * 10 };
        else if ( name == QLatin1String("RA") ) state = { Attenuator, value * 6 };
        else if ( name == QLatin1String("GT") ) state = { Agc, yaesuAgcToHamlib(value) };
        else if ( name == QLatin1String("NB") ) state = { NoiseBlanker, value ? 1 : 0 };
        else                                    state = { NoiseReduction, value ? 1 : 0 };
        return true;
    }

    case OmniRigMeters::KenwoodFamily:
    {
        // PA answers main then sub, RA two digits for main then two for sub
        static const QRegularExpression preamp(QStringLiteral("^PA(\\d)\\d?;"));
        static const QRegularExpression att(QStringLiteral("^RA\\d(\\d)"));
        static const QRegularExpression other(QStringLiteral("^(GC|NB|NR)(\\d);"));
        QRegularExpressionMatch match;

        if ( ( match = preamp.match(text) ).hasMatch() )
            state = { Preamp, match.captured(1).toInt() ? 12 : 0 };
        else if ( ( match = att.match(text) ).hasMatch() )
            state = { Attenuator, match.captured(1).toInt() ? 12 : 0 };
        else if ( ( match = other.match(text) ).hasMatch() )
        {
            const int value = match.captured(2).toInt();

            if ( match.captured(1) == QLatin1String("GC") )
                state = { Agc, value == 1 ? AGC_SLOW : value == 2 ? AGC_FAST : AGC_OFF };
            else if ( match.captured(1) == QLatin1String("NB") )
                state = { NoiseBlanker, value ? 1 : 0 };
            else
                state = { NoiseReduction, value ? 1 : 0 };
        }
        else
            return false;

        return true;
    }

    case OmniRigMeters::IcomFamily:
    {
        // FE FE E0 <rig> <code> <one byte> FD, after the echo
        for ( Control control : { Preamp, Attenuator, Agc, NoiseBlanker, NoiseReduction } )
        {
            const QByteArray head = QByteArray::fromHex("FEFEE0") + rig.civAddress + icomCode(control);
            const int at = reply.indexOf(head);
            const int data = at + head.size();

            if ( at < 0 || reply.size() < data + 2
                 || static_cast<unsigned char>(reply.at(data + 1)) != 0xFD )
                continue;

            const unsigned char byte = static_cast<unsigned char>(reply.at(data));

            switch ( control )
            {
            case Preamp:         state = { Preamp, byte * 10 }; break;
            case Attenuator:     state = { Attenuator, fromBcd(byte) }; break;
            case Agc:            state = { Agc, icomAgcToHamlib(byte) }; break;
            case NoiseBlanker:   state = { NoiseBlanker, byte ? 1 : 0 }; break;
            case NoiseReduction: state = { NoiseReduction, byte ? 1 : 0 }; break;
            }

            return true;
        }

        return false;
    }

    default:
        return false;
    }
}
