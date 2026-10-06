#ifndef QLOG_RIG_OMNIRIGMETERS_H
#define QLOG_RIG_OMNIRIGMETERS_H

#include <QByteArray>
#include <QList>
#include <QString>

/* Meters OmniRig does not read (S, power, SWR, ALC), polled via custom CAT
   commands and converted to Hamlib units with Hamlib's calibration tables.
   Yaesu text protocol, Icom, Kenwood TS-480/590/2000; binary Yaesus have none. */
namespace OmniRigMeters
{
    enum Family { UnknownFamily, YaesuFamily, KenwoodFamily, IcomFamily };
    enum Meter { Strength, Power, Swr, Alc };

    struct RigInfo
    {
        Family family = UnknownFamily;
        bool strength = false;
        bool transmitMeters = false;
        // which of the family's calibration tables applies
        bool ftdx101 = false;
        bool ic705 = false;
        // FTDX101D rig file also covers the 200 W MP: profile power decides
        double maxWatts = 100.0;
        // Icom only
        char civAddress = 0;
        bool civEcho = true;
    };

    struct Request
    {
        QByteArray command;
        int replyLength = 0;
        QByteArray replyEnd;
    };

    struct Reading
    {
        Meter meter;
        double value;
    };

    RigInfo describe(const QString &rigType, const QString &rigFile, double maxWatts);

    // an empty command when the rig cannot be read this way
    Request strengthRequest(const RigInfo &rig);

    // sent one per poll in turn, so commands never pile up on the rig
    QList<Request> transmitRequests(const RigInfo &rig);

    // Kenwood RM carries three meters; Kenwood SM0 is power while transmitting
    QList<Reading> parse(const RigInfo &rig, const QByteArray &reply, bool transmitting);
}

#endif // QLOG_RIG_OMNIRIGMETERS_H
