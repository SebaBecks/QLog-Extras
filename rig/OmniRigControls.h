#ifndef QLOG_RIG_OMNIRIGCONTROLS_H
#define QLOG_RIG_OMNIRIGCONTROLS_H

#include <QList>
#include <QString>

#include "OmniRigMeters.h"

/* Preamp/ATT/AGC/NB/NR via OmniRig custom CAT commands (Yaesu text protocol
   with FTDX101 steps, Icom with IC-7300 steps, Kenwood TS-590).
   Units as on the rigctld path: dB, Hamlib rig_agc_level_e, 0/1. */
namespace OmniRigControls
{
    enum Control { Preamp, Attenuator, Agc, NoiseBlanker, NoiseReduction };

    struct State
    {
        Control control;
        int value;
    };

    bool supported(const OmniRigMeters::RigInfo &rig);

    OmniRigMeters::Request readRequest(const OmniRigMeters::RigInfo &rig, Control control);
    OmniRigMeters::Request writeRequest(const OmniRigMeters::RigInfo &rig, Control control, int value);

    // AGC off is left out so a click cannot land on it by accident
    QList<int> steps(const OmniRigMeters::RigInfo &rig, Control control);

    // the rig's own name for a preamp setting
    QString preampName(const OmniRigMeters::RigInfo &rig, int db);

    // false when the reply is not an answer to one of the read requests
    bool parse(const OmniRigMeters::RigInfo &rig, const QByteArray &reply, State &state);
}

#endif // QLOG_RIG_OMNIRIGCONTROLS_H
