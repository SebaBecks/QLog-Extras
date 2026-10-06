#ifndef QLOG_UI_RIGPANELWIDGET_H
#define QLOG_UI_RIGPANELWIDGET_H

#include <QWidget>
#include <QStringList>
#include <QPointer>
#include <QHash>
#include <QByteArray>

#include "rig/Rig.h"
#include "rig/OmniRigMeters.h"
#include "rig/OmniRigControls.h"
#include "ui/component/ShutdownAwareWidget.h"

class MeterScaleWidget;
class NewContactWidget;
class QLabel;
class QsoRecorder;
class RigStateButton;
class QTimer;
class OmniRigClient;
class RigctldClient;

namespace Ui {
class RigPanelWidget;
}

/* Extended rig view. Frequency/mode/VFO/split/PTT come from Rig signals (any
   driver); meters and receiver settings from rigctld (Hamlib with Share Rig)
   or from OmniRig custom commands. Other drivers leave those blank. */
class RigPanelWidget : public QWidget, public ShutdownAwareWidget
{
    Q_OBJECT

public:
    explicit RigPanelWidget(QWidget *parent = nullptr);
    ~RigPanelWidget();
    void finalizeBeforeAppExit() override;
    // recordings are named after the contact being worked
    void registerContactWidget(const NewContactWidget *widget);

public slots:
    void rigConnectHandler();
    void rigDisconnectHandler();

private slots:
    void frequencyChanged(VFOID vfoid, double vfoFreq, double ritFreq, double xitFreq);
    void modeChanged(VFOID vfoid, const QString &rawMode, const QString &mode,
                     const QString &subMode, qint32 width);
    void vfoChanged(VFOID vfoid, const QString &vfo);
    void splitChanged(VFOID vfoid, bool split);
    void pttChanged(VFOID vfoid, bool ptt);
    void startCycle();
    void rigctldResults(const QStringList &values);
    void rigctldFailed(const QString &reason);
    void swapVfoClicked();
    void splitClicked();
    void tunerClicked();
    void preampClicked();
    void attClicked();
    void agcClicked();
    void nbClicked();
    void nrClicked();
    void recordClicked();
    void recordMenu(const QPoint &where);
    void recordingChanged(bool on);
    void showRecordingTime();
    void pollOmniRig();
    void omniRigReply(const QByteArray &command, const QByteArray &reply);

private:
    void applyFonts();
    void clearReadings();
    void scheduleCycle(int delayMs);
    // dB relative to S9 (Hamlib STRENGTH), before S cal/S9+ corrections
    void showStrength(int db);
    // per-profile S cal and S9+ settings
    QString meterCorrectionKey(const QString &key) const;
    void loadMeterCorrections();
    void showControlState(OmniRigControls::Control control, int value,
                          const QString &preampText = QString());
    bool usesOmniRigControls() const;
    void stepOmniRigControl(OmniRigControls::Control control, int current);
    void startOmniRigSweep();
    void readNextOmniRigControl();
    void showSwr(double swr);
    void showPower(double watts);
    void updateVfoRows();
    QString otherVfoName() const;
    void setActiveVfo(bool isB);
    bool omniRigSkips(const OmniRigMeters::Request &request, qint64 now) const;
    bool askOmniRig(const OmniRigMeters::Request &request);
    void showSupply();
    // S reading next to the frequency of the receiving VFO
    void showStrengthInline();
    void clearRigState();
    void buildStateButtons();
    // write, then read back, so the button shows what the rig actually did
    void setRigLevel(const QString &name, int value);
    void setRigFunc(const QString &name, bool on);
    // sent at the head of the next poll batch, not as a separate request
    void queueRigctldCommand(const QString &command, int lines = 1);
    // read all slow values at once right after connecting
    void primeSlowReadings();
    void loadRigctldSteps(int model);
    int nextStep(const QList<int> &steps, int current) const;
    QString preampName(int value) const;
    // opens the card so the pre-roll buffer fills before REC is pressed
    void applyRecordingSettings();
    QString recordingLabel() const;
    QString recordTip() const;

    Ui::RigPanelWidget *ui;
    double currentFreq;
    bool splitEnabled;
    bool extrasAvailable;
    int maxPower;
    int pollCounter;
    MeterScaleWidget *sMeterScale;
    MeterScaleWidget *pwrScale;
    MeterScaleWidget *swrScale;
    QStringList batch;
    QList<int> batchLines;
    RigctldClient *rigctld;
    enum MeterSource { NoMeters, RigctldMeters };
    MeterSource meterSource;
    bool swrWarning;
    bool keyed;
    // Rig publishes only the active VFO; the other one is polled separately
    bool activeVfoIsB;
    QString activeMode;
    QString otherFreq;
    QString otherMode;
    QString strengthText;
    QStringList pendingWrites;
    QList<int> pendingWriteLines;
    double supplyVolts;
    double supplyAmps;
    bool haveSupply;
    RigStateButton *preButton;
    RigStateButton *attButton;
    RigStateButton *agcButton;
    RigStateButton *nbButton;
    RigStateButton *nrButton;
    RigStateButton *vfoButton;
    RigStateButton *splitButton;
    RigStateButton *tunerButton;
    // last reported values; a click steps from these
    int preampDb;
    int attDb;
    int agcMode;
    // rigctld values for PRE, ATT and AGC, from Hamlib's caps for the model
    QList<int> preampSteps;
    QList<int> attSteps;
    QList<int> agcSteps;
    bool yaesuPreampNames = true;
    // get_vfo_info for the other VFO is safe on this rig (not on Icoms)
    bool otherVfoReadable = true;
    // a second receiver (Main/Sub): the other VFO is shown outside split too
    bool dualReceiver = true;
    // the rig layer reports the active VFO; otherwise A/B is followed here
    bool rigReportsVfo = false;
    RigStateButton *recButton;
    QsoRecorder *recorder;
    QTimer *recordTimer;
    // guarded: the contact widget may be destroyed first on exit
    QPointer<const NewContactWidget> contact;
    /* One timer, not a single shot per reply, so a reconnect restarts the
       wait instead of starting a second poll loop. */
    QTimer *cycleTimer = nullptr;
    // number of queued commands at the head of the batch
    int writesInBatch = 0;
    // log mode name for the file name (activeMode is the rig's raw mode)
    QString logMode;
    // second OmniRig client, open only with an OmniRig driver
    OmniRigClient *omnirig;
    QTimer *omnirigTimer;
    OmniRigMeters::RigInfo omniRigInfo;
    // time of the pending meter request, so a silent rig is not flooded
    qint64 meterAskedAt;
    int transmitTurn;
    QString meterProfile;
    // settings to read back through OmniRig before the next meter
    QList<OmniRigMeters::Request> omniRigReads;
    int controlTurn;
    int sweepLeft;
    qint64 lastSweepAt;
    /* Pending OmniRig request and PTT state when sent: Kenwood SM0 means
       strength on RX and power on TX. */
    QByteArray omniRigAsked;
    bool omniRigAskedKeyed = false;
    // pending answer was asked before a PTT change, discard it
    bool omniRigStale = false;
    /* Unanswered-in-a-row counts and back-off deadlines per command. A rig
       without the command never answers and OmniRig waits out its timeout,
       blocking the queue. */
    QHash<QByteArray, int> omniRigMisses;
    QHash<QByteArray, qint64> omniRigQuietUntil;
};

#endif // QLOG_UI_RIGPANELWIDGET_H
