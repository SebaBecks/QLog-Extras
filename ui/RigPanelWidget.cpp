#include <QTimer>
#include <QDateTime>
#include <QtMath>
#include <QFont>
#include <QLabel>
#include <QAbstractButton>
#include <QFontMetrics>
#include <QSettings>
#include <QPainter>
#include <QGridLayout>
#include <QMenu>
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#include <QDir>

#include "RigPanelWidget.h"
#include "ui_RigPanelWidget.h"
#include "rig/RigctldClient.h"
#include "rig/OmniRigClient.h"
#include "data/RigProfile.h"
#include "data/BandPlan.h"
#include "rig/QsoRecorder.h"
#include "ui/NewContactWidget.h"
#include "ui/RigRecordingDialog.h"
#include "core/debug.h"
#include "rig/drivers/HamlibCompat.h"

#include <hamlib/rig.h>

MODULE_IDENTIFICATION("qlog.ui.rigpanelwidget");

/* 38400 baud CAT: 5 requests per 250 ms saturated it (frozen readings,
   garbled frequency). Keep to one or two per second. */
#define RX_INTERVAL         1000
// no S meter on TX, so power and SWR can be polled faster
#define TX_INTERVAL         300
/* Settings and the other VFO ride along with the meters in groups, one group
   every 2nd poll (full set in ~6 s). Bursts in one pass hurt CAT, not totals. */
#define SLOW_EVERY_NTH_POLL 2
#define SLOW_GROUPS         3
// cap on queued commands per pass, same reason as the groups
#define MAX_QUEUED_PER_POLL 4
// preamp, attenuator, AGC, noise blanker, noise reduction
#define OMNIRIG_CONTROLS    5
// sweep period for reading them through OmniRig on RX
#define OMNIRIG_SWEEP_MS    2000
#define REPLY_TIMEOUT       3000
// after this many misses in a row a command is paused for OMNIRIG_QUIET_MS
#define OMNIRIG_MAX_MISSES  3
#define OMNIRIG_QUIET_MS    60000
#define SETTINGS_KEY_CAL    "rigpanel/smeteroffset"
#define SETTINGS_KEY_GAIN   "rigpanel/smetergain"

// painted ticks stay aligned with the bar at any dock width
class MeterScaleWidget : public QWidget
{
public:
    struct Tick
    {
        double fraction;
        QString label;
    };

    explicit MeterScaleWidget(QWidget *parent = nullptr) :
        QWidget(parent)
    {
        setFixedHeight(15);
    }

    void setTicks(const QVector<Tick> &newTicks)
    {
        ticks = newTicks;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if ( ticks.isEmpty() )
            return;

        QPainter painter(this);
        QFont small = painter.font();
        small.setPointSizeF(small.pointSizeF() * 0.75);
        painter.setFont(small);
        painter.setPen(palette().color(QPalette::WindowText));

        const int usable = width() - 1;

        for ( const Tick &tick : ticks )
        {
            const int x = static_cast<int>(tick.fraction * usable);
            painter.drawLine(x, 0, x, 4);

            const int textWidth = painter.fontMetrics().horizontalAdvance(tick.label);
            painter.drawText(qBound(0, x - textWidth / 2, width() - textWidth),
                             height() - 1, tick.label);
        }
    }

private:
    QVector<Tick> ticks;
};


/* Custom-painted button with a caption and a value line. A flat QPushButton
   has one font size and an almost invisible checked state in this style. */
class RigStateButton : public QAbstractButton
{
public:
    explicit RigStateButton(const QString &caption, QWidget *parent = nullptr) :
        QAbstractButton(parent),
        active(false),
        alarm(false)
    {
        setText(caption);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
    }

    void setDetail(const QString &value)
    {
        if ( detail == value )
            return;

        detail = value;
        updateGeometry();
        update();
    }

    // widest expected text, so the row does not resize as values change
    void reserveDetail(const QString &widest)
    {
        reserved = widest;
        updateGeometry();
    }

    void setActive(bool on)
    {
        if ( active == on )
            return;

        active = on;
        update();
    }

    bool isActive() const { return active; }

    // red caption and border (REC), red fill too while active
    void setAlarm(bool on)
    {
        alarm = on;
        update();
    }

    QSize sizeHint() const override
    {
        const QFontMetrics caps(captionFont());
        const QFontMetrics dets(detailFont());

        const int width = qMax(caps.horizontalAdvance(text()),
                               qMax(dets.horizontalAdvance(detail),
                                    dets.horizontalAdvance(reserved)));

        // always reserve the second line so captions stay aligned
        return QSize(width + 14, caps.height() + dets.height() + 8);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QColor border(0x9a, 0x9a, 0x9a);
        QColor fill(0xf4, 0xf4, 0xf4);
        QColor ink(0x6b, 0x6b, 0x6b);

        // Material 800 green for a setting, red for record
        if ( alarm )
        {
            border = QColor(0xc6, 0x28, 0x28);
            ink = border;

            if ( active )
                fill = QColor(0xfb, 0xe3, 0xe3);
        }
        else if ( active )
        {
            border = QColor(0x2e, 0x7d, 0x32);
            fill = QColor(0xdf, 0xf0, 0xdf);
            ink = border;
        }

        if ( !isEnabled() )
        {
            border = QColor(0xdc, 0xdc, 0xdc);
            fill = QColor(0xfb, 0xfb, 0xfb);
            ink = QColor(0xc2, 0xc2, 0xc2);
        }
        else if ( isDown() )
            fill = fill.darker(110);

        painter.setPen(QPen(border, 1));
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                3.0, 3.0);

        const QFontMetrics caps(captionFont());

        painter.setPen(ink);
        painter.setFont(captionFont());
        painter.drawText(QRect(0, 3, width(), caps.height()),
                         Qt::AlignHCenter | Qt::AlignTop, text());

        if ( detail.isEmpty() )
            return;

        const QFontMetrics dets(detailFont());

        painter.setFont(detailFont());
        painter.drawText(QRect(0, 2 + caps.height(), width(), dets.height()),
                         Qt::AlignHCenter | Qt::AlignTop, detail);
    }

private:
    QFont captionFont() const
    {
        QFont result = font();
        result.setBold(true);
        return result;
    }

    QFont detailFont() const
    {
        QFont result = font();
        result.setPointSizeF(result.pointSizeF() * 0.78);
        return result;
    }

    QString detail;
    QString reserved;
    bool active;
    bool alarm;
};

namespace {

// S1-S9 on the first half, +60 dB on the second, as on a rig meter
QVector<MeterScaleWidget::Tick> sMeterTicks()
{
    FCT_IDENTIFICATION;

    QVector<MeterScaleWidget::Tick> ticks;

    for ( int s = 1; s <= 9; s += 2 )
        ticks << MeterScaleWidget::Tick{ s / 9.0 / 2.0, QString::number(s) };

    for ( int db = 10; db <= 60; db += 10 )
        ticks << MeterScaleWidget::Tick{ 0.5 + db / 60.0 / 2.0, QString("+%1").arg(db) };

    return ticks;
}

// Hamlib STRENGTH: dB relative to S9, 6 dB per S unit
int sBarFromDb(int db)
{
    FCT_IDENTIFICATION;

    const double sUnits = 9.0 + db / 6.0;

    return ( sUnits <= 9.0 ) ? qBound(0, static_cast<int>(sUnits / 9.0 * 50.0), 50)
                             : qBound(50, 50 + static_cast<int>(db / 60.0 * 50.0), 100);
}

// S units below S9, 5 dB steps above (the step used in reports)
QString sTextFromDb(int db)
{
    FCT_IDENTIFICATION;

    if ( db < 0 )
        return QString("S%1").arg(qBound(0, qRound(9.0 + db / 6.0), 9));

    return QString("S9+%1").arg(qRound(db / 5.0) * 5);
}

// 7.170.020, grouped as on the rig display
QString formatFrequency(double mhz)
{
    FCT_IDENTIFICATION;

    const qint64 hz = qRound64(mhz * 1.0e6);

    return QString("%1.%2.%3")
            .arg(hz / 1000000)
            .arg((hz / 1000) % 1000, 3, 10, QChar('0'))
            .arg(hz % 1000, 3, 10, QChar('0'));
}

// bar spans SWR 1 to 3; beyond that only "too high" matters
QVector<MeterScaleWidget::Tick> swrTicks()
{
    FCT_IDENTIFICATION;

    QVector<MeterScaleWidget::Tick> ticks;

    ticks << MeterScaleWidget::Tick{ 0.25, QStringLiteral("1.5") }
          << MeterScaleWidget::Tick{ 0.50, QStringLiteral("2") }
          << MeterScaleWidget::Tick{ 0.75, QStringLiteral("2.5") }
          << MeterScaleWidget::Tick{ 1.00, QStringLiteral("3") };

    return ticks;
}

// Hamlib rig_agc_level_e
QString agcName(int mode)
{
    FCT_IDENTIFICATION;

    switch ( mode )
    {
    case 0:  return QStringLiteral("Off");
    case 1:  return QStringLiteral("SFast");
    case 2:  return QStringLiteral("Fast");
    case 3:  return QStringLiteral("Slow");
    case 4:  return QStringLiteral("User");
    case 5:  return QStringLiteral("Med");
    case 6:  return QStringLiteral("Auto");
    case 7:  return QStringLiteral("Long");
    case 8:  return QStringLiteral("On");
    default: return QString::number(mode);
    }
}

QVector<MeterScaleWidget::Tick> powerTicks(int maxWatts)
{
    FCT_IDENTIFICATION;

    QVector<MeterScaleWidget::Tick> ticks;

    for ( int i = 1; i <= 4; i++ )
        ticks << MeterScaleWidget::Tick{ i / 4.0, QString::number(maxWatts * i / 4) };

    return ticks;
}

} // namespace

RigPanelWidget::RigPanelWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::RigPanelWidget),
    currentFreq(0.0),
    splitEnabled(false),
    extrasAvailable(false),
    maxPower(1),
    pollCounter(0),
    rigctld(new RigctldClient(this)),
    meterSource(NoMeters),
    swrWarning(false),
    keyed(false),
    activeVfoIsB(false),
    supplyVolts(0.0),
    supplyAmps(0.0),
    haveSupply(false),
    preButton(nullptr),
    attButton(nullptr),
    agcButton(nullptr),
    nbButton(nullptr),
    nrButton(nullptr),
    vfoButton(nullptr),
    splitButton(nullptr),
    tunerButton(nullptr),
    preampDb(0),
    attDb(0),
    agcMode(0),
    swrScale(nullptr),
    recButton(nullptr),
    recorder(new QsoRecorder(this)),
    recordTimer(new QTimer(this)),
    contact(nullptr),
    omnirig(new OmniRigClient(this)),
    omnirigTimer(new QTimer(this)),
    meterAskedAt(0),
    transmitTurn(0),
    controlTurn(0),
    sweepLeft(0),
    lastSweepAt(0)
{
    FCT_IDENTIFICATION;

    ui->setupUi(this);
    applyFonts();
    buildStateButtons();

    cycleTimer = new QTimer(this);
    cycleTimer->setSingleShot(true);
    connect(cycleTimer, &QTimer::timeout, this, &RigPanelWidget::startCycle);

    // white below S9, light red above, red line at S9 (half scale)
    ui->sMeterBar->setStyleSheet(
        "QProgressBar { border: 1px solid palette(mid); background: "
        "qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        " stop:0 white, stop:0.496 white,"
        " stop:0.497 red, stop:0.503 red,"
        " stop:0.504 #ffdede, stop:1 #ffdede); }"
        "QProgressBar::chunk { background-color: #3daee9; }");

    // rows 1, 3 and 5 of the meter grid are reserved for the scales
    sMeterScale = new MeterScaleWidget(this);
    sMeterScale->setTicks(sMeterTicks());
    ui->meterLayout->addWidget(sMeterScale, 1, 1);

    pwrScale = new MeterScaleWidget(this);
    ui->meterLayout->addWidget(pwrScale, 3, 1);

    swrScale = new MeterScaleWidget(this);
    swrScale->setTicks(swrTicks());
    ui->meterLayout->addWidget(swrScale, 5, 1);

    clearReadings();
    clearRigState();
    updateVfoRows();

    /* Hamlib's calibration reads low on some rigs; user offset/gain per rig
       profile (6 dB = 1 S unit). */
    loadMeterCorrections();

    connect(ui->calSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value)
    {
        QSettings settings;
        settings.setValue(meterCorrectionKey(SETTINGS_KEY_CAL), value);
    });

    connect(ui->gainSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value)
    {
        QSettings settings;
        settings.setValue(meterCorrectionKey(SETTINGS_KEY_GAIN), value);
    });

    connect(rigctld, &RigctldClient::results, this, &RigPanelWidget::rigctldResults);
    connect(rigctld, &RigctldClient::failed, this, &RigPanelWidget::rigctldFailed);
    connect(omnirigTimer, &QTimer::timeout, this, &RigPanelWidget::pollOmniRig);
    connect(omnirig, &OmniRigClient::customReply, this, &RigPanelWidget::omniRigReply);

    Rig *rig = Rig::instance();

    connect(rig, &Rig::frequencyChanged, this, &RigPanelWidget::frequencyChanged);
    connect(rig, &Rig::modeChanged, this, &RigPanelWidget::modeChanged);
    connect(rig, &Rig::vfoChanged, this, &RigPanelWidget::vfoChanged);
    connect(rig, &Rig::splitChanged, this, &RigPanelWidget::splitChanged);
    connect(rig, &Rig::pttChanged, this, &RigPanelWidget::pttChanged);
    connect(rig, &Rig::rigConnected, this, &RigPanelWidget::rigConnectHandler);
    connect(rig, &Rig::rigDisconnected, this, &RigPanelWidget::rigDisconnectHandler);

    if ( rig->isRigConnected() )
        rigConnectHandler();

    connect(recorder, &QsoRecorder::recordingChanged, this, &RigPanelWidget::recordingChanged);
    connect(recorder, &QsoRecorder::saved, this, [this](const QString &path)
    {
        recButton->setToolTip(recordTip() + tr("\n\nLast saved: %1").arg(path));
    });
    connect(recorder, &QsoRecorder::failed, this, [this](const QString &reason)
    {
        recButton->setDetail(tr("error"));
        recButton->setToolTip(recordTip() + QStringLiteral("\n\n") + reason);
    });
    connect(recordTimer, &QTimer::timeout, this, &RigPanelWidget::showRecordingTime);
    recordTimer->setInterval(500);

    applyRecordingSettings();
}

RigPanelWidget::~RigPanelWidget()
{
    FCT_IDENTIFICATION;

    // the recorder outlives the panel briefly; stop its signals first
    recorder->disconnect(this);

    if ( recorder->isRecording() )
        recorder->stop(recordingLabel());

    delete ui;
}

// stop while the contact widget still exists to name the file
void RigPanelWidget::finalizeBeforeAppExit()
{
    FCT_IDENTIFICATION;

    if ( recorder->isRecording() )
        recorder->stop(recordingLabel());
}

void RigPanelWidget::applyFonts()
{
    FCT_IDENTIFICATION;

    const QFont base = font();

    QFont freqFont = base;
    freqFont.setPointSize(base.pointSize() * 3);
    freqFont.setBold(true);
    ui->aFreqLabel->setFont(freqFont);

    QFont modeFont = base;
    modeFont.setPointSize(base.pointSize() * 2);
    modeFont.setBold(true);
    ui->aModeLabel->setFont(modeFont);

    // row B stays smaller whichever VFO is active; colour marks the live row
    QFont subFreqFont = base;
    subFreqFont.setPointSize(qRound(base.pointSize() * 1.6));
    ui->bFreqLabel->setFont(subFreqFont);

    QFont subModeFont = base;
    subModeFont.setPointSize(qRound(base.pointSize() * 1.2));
    ui->bModeLabel->setFont(subModeFont);

    QFont unitFont = base;
    unitFont.setPointSizeF(base.pointSizeF() * 0.85);
    ui->aUnitLabel->setFont(unitFont);
    ui->bUnitLabel->setFont(unitFont);
    ui->splitLabel->setFont(unitFont);

    QFont capFont = base;
    capFont.setBold(true);
    ui->aCaption->setFont(capFont);
    ui->bCaption->setFont(capFont);

    // amber: green already marks the live VFO, blue marks split
    QFont strengthFont = base;
    strengthFont.setPointSizeF(base.pointSizeF() * 1.9);
    strengthFont.setBold(true);
    ui->aStrengthLabel->setFont(strengthFont);
    ui->bStrengthLabel->setFont(strengthFont);

    for ( QLabel *label : { ui->aStrengthLabel, ui->bStrengthLabel } )
        label->setStyleSheet(QStringLiteral("QLabel { color: #ef6c00;"
                                            " padding-right: 14px }"));
}

void RigPanelWidget::clearReadings()
{
    FCT_IDENTIFICATION;

    ui->sMeterBar->setValue(0);
    ui->pwrBar->setValue(0);
    ui->swrBar->setValue(0);
    strengthText.clear();
    ui->aStrengthLabel->clear();
    ui->bStrengthLabel->clear();
    ui->sMeterValue->setText(QStringLiteral("--"));
    ui->pwrValue->setText(QStringLiteral("--"));
    ui->swrValue->setText(QStringLiteral("--"));
}

void RigPanelWidget::rigConnectHandler()
{
    FCT_IDENTIFICATION;

    const RigProfile &profile = RigProfilesManager::instance()->getCurProfile1();

    meterProfile = profile.profileName;
    loadMeterCorrections();

    // recording settings are per rig profile
    RigRecordingDialog::setProfile(profile.profileName);
    applyRecordingSettings();

    pollCounter = 0;
    otherFreq.clear();
    otherMode.clear();
    clearRigState();
    updateVfoRows();

    if ( profile.driver == Rig::HAMLIB_DRIVER && profile.shareRigctld )
    {
        meterSource = RigctldMeters;
        rigctld->setEndpoint(QStringLiteral("127.0.0.1"), profile.rigctldPort);

        // rigctld has no max power; use the profile's
        maxPower = qMax(1, static_cast<int>(profile.defaultPWR));
        ui->pwrBar->setMaximum(maxPower);
        pwrScale->setTicks(powerTicks(maxPower));
    }
    else
        meterSource = NoMeters;

    extrasAvailable = ( meterSource != NoMeters );

    ui->sourceLabel->setText(extrasAvailable
                             ? QString()
                             : tr("meters unavailable with this driver"));

    // split goes through the Rig layer (any driver), the rest only via rigctld
    const bool viaRigctld = ( meterSource == RigctldMeters );

    for ( RigStateButton *button : { preButton, attButton, agcButton,
                                     nbButton, nrButton, vfoButton, tunerButton } )
        button->setEnabled(viaRigctld);

    splitButton->setEnabled(viaRigctld || profile.getSplitInfo);

    if ( viaRigctld )
    {
        loadRigctldSteps(profile.model);
        primeSlowReadings();
    }

    if ( extrasAvailable )
        scheduleCycle(RX_INTERVAL);

    // the Rig layer passes the other VFO only in split, so ask OmniRig directly
    const int omniRigVersion = ( profile.driver == Rig::OMNIRIG_DRIVER ) ? 1
                             : ( profile.driver == Rig::OMNIRIGV2_DRIVER ) ? 2 : 0;

    if ( omniRigVersion && omnirig->open(omniRigVersion, profile.model) )
    {
        omniRigInfo = OmniRigMeters::describe(omnirig->rigType(), omnirig->rigFile(),
                                              profile.defaultPWR);
        meterAskedAt = 0;
        transmitTurn = 0;
        omniRigStale = false;
        omniRigMisses.clear();
        omniRigQuietUntil.clear();

        if ( omniRigInfo.strength || omniRigInfo.transmitMeters )
            ui->sourceLabel->setText(QString());

        if ( omniRigInfo.transmitMeters )
        {
            maxPower = qMax(1, static_cast<int>(profile.defaultPWR));
            ui->pwrBar->setMaximum(maxPower);
            pwrScale->setTicks(powerTicks(maxPower));
        }

        // read each setting once now, one per poll, so the buttons fill in
        omniRigReads.clear();
        controlTurn = 0;
        sweepLeft = 0;
        lastSweepAt = 0;

        if ( OmniRigControls::supported(omniRigInfo) )
        {
            for ( RigStateButton *button : { preButton, attButton, agcButton, nbButton, nrButton } )
                button->setEnabled(true);

            for ( OmniRigControls::Control control : { OmniRigControls::Preamp,
                                                       OmniRigControls::Attenuator,
                                                       OmniRigControls::Agc,
                                                       OmniRigControls::NoiseBlanker,
                                                       OmniRigControls::NoiseReduction } )
                omniRigReads << OmniRigControls::readRequest(omniRigInfo, control);
        }

        omnirigTimer->start(keyed ? TX_INTERVAL : RX_INTERVAL);
    }
}

void RigPanelWidget::rigDisconnectHandler()
{
    FCT_IDENTIFICATION;

    extrasAvailable = false;
    currentFreq = 0.0;

    cycleTimer->stop();
    omnirigTimer->stop();
    omnirig->close();
    omniRigInfo = OmniRigMeters::RigInfo();
    omniRigReads.clear();
    meterAskedAt = 0;
    omniRigStale = false;
    sweepLeft = 0;

    keyed = false;
    splitEnabled = false;
    activeVfoIsB = false;
    rigReportsVfo = false;
    otherFreq.clear();
    otherMode.clear();
    activeMode.clear();
    pendingWrites.clear();
    pendingWriteLines.clear();
    clearRigState();
    updateVfoRows();

    for ( RigStateButton *button : { preButton, attButton, agcButton, nbButton,
                                     nrButton, vfoButton, splitButton, tunerButton } )
        button->setEnabled(false);

    ui->filterCombo->clear();
    ui->sourceLabel->setText(tr("rig disconnected"));
    clearReadings();
}

void RigPanelWidget::frequencyChanged(VFOID vfoid, double vfoFreq, double, double)
{
    FCT_IDENTIFICATION;

    // in split, OmniRig/flrig report the TX VFO as VFO2: show it in the other row
    if ( vfoid == VFO2 )
    {
        // with rigctld the cycle reads the TX frequency itself (i)
        if ( splitEnabled && meterSource == RigctldMeters )
            return;

        otherFreq = vfoFreq > 0.0 ? formatFrequency(vfoFreq) : QString();
        updateVfoRows();
        return;
    }

    // Hamlib's client reports the TX VFO as current in split; the cycle reads RX
    if ( splitEnabled && meterSource == RigctldMeters )
        return;

    currentFreq = vfoFreq;
    updateVfoRows();
}

void RigPanelWidget::modeChanged(VFOID, const QString &rawMode, const QString &mode,
                                 const QString &subMode, qint32 width)
{
    FCT_IDENTIFICATION;

    // show the rig's own mode name
    activeMode = rawMode.isEmpty() ? (subMode.isEmpty() ? mode : subMode) : rawMode;
    logMode = mode;
    updateVfoRows();

    // width comes with the signal; no need for a two-line get_mode via rigctld
    if ( meterSource != RigctldMeters || width <= 0 )
        return;

    const QString &text = QString::number(width);

    if ( ui->filterCombo->findText(text) < 0 )
        ui->filterCombo->addItem(text);

    ui->filterCombo->blockSignals(true);
    ui->filterCombo->setCurrentText(text);
    ui->filterCombo->blockSignals(false);
}

// most rigs report VFOA/VFOB, dual-receiver Yaesus Main/Sub (Sub = B)
void RigPanelWidget::vfoChanged(VFOID, const QString &vfo)
{
    FCT_IDENTIFICATION;

    if ( vfo.isEmpty() || vfo.contains(QStringLiteral("curr"), Qt::CaseInsensitive) )
        return;

    rigReportsVfo = true;

    // label rows as the rig does
    const bool mainSub = vfo.contains(QStringLiteral("Main"), Qt::CaseInsensitive)
                         || vfo.contains(QStringLiteral("Sub"), Qt::CaseInsensitive);

    ui->aCaption->setText(mainSub ? tr("MAIN") : tr("VFO A"));
    ui->bCaption->setText(mainSub ? tr("SUB") : tr("VFO B"));

    setActiveVfo(vfo.contains(QStringLiteral("Sub"), Qt::CaseInsensitive)
                 || vfo.endsWith(QChar('B')));
}

void RigPanelWidget::setActiveVfo(bool isB)
{
    FCT_IDENTIFICATION;

    if ( isB == activeVfoIsB )
        return;

    activeVfoIsB = isB;

    // the stored "other" VFO is now the active one; drop it until re-read
    otherFreq.clear();
    otherMode.clear();
    updateVfoRows();
}

QString RigPanelWidget::otherVfoName() const
{
    FCT_IDENTIFICATION;

    return activeVfoIsB ? QStringLiteral("VFOA") : QStringLiteral("VFOB");
}

/* Row A always on top; the idle row is grey. The TX box marks the VFO that
   transmits, which split moves to the other one. */
void RigPanelWidget::updateVfoRows()
{
    FCT_IDENTIFICATION;

    QLabel *liveFreq = activeVfoIsB ? ui->bFreqLabel : ui->aFreqLabel;
    QLabel *liveMode = activeVfoIsB ? ui->bModeLabel : ui->aModeLabel;
    QLabel *liveUnit = activeVfoIsB ? ui->bUnitLabel : ui->aUnitLabel;
    QLabel *liveCap  = activeVfoIsB ? ui->bCaption   : ui->aCaption;
    QLabel *idleFreq = activeVfoIsB ? ui->aFreqLabel : ui->bFreqLabel;
    QLabel *idleMode = activeVfoIsB ? ui->aModeLabel : ui->bModeLabel;
    QLabel *idleUnit = activeVfoIsB ? ui->aUnitLabel : ui->bUnitLabel;
    QLabel *idleCap  = activeVfoIsB ? ui->aCaption   : ui->bCaption;

    const QString blank = QStringLiteral("--.---.---");
    const QString grey = QStringLiteral("QLabel { color: #9a9a9a }");

    // a single receiver has no other VFO worth showing outside split
    const bool showOther = ( meterSource != RigctldMeters ) || dualReceiver || splitEnabled;

    liveFreq->setText(currentFreq > 0.0 ? formatFrequency(currentFreq) : blank);
    liveMode->setText(activeMode);
    idleFreq->setText(showOther && !otherFreq.isEmpty() ? otherFreq : blank);
    idleMode->setText(showOther ? otherMode : QString());

    liveCap->setStyleSheet(QStringLiteral("QLabel { color: #2e7d32 }"));

    for ( QLabel *label : { liveFreq, liveMode, liveUnit } )
        label->setStyleSheet(QString());

    for ( QLabel *label : { idleFreq, idleMode, idleUnit, idleCap } )
        label->setStyleSheet(grey);

    ui->splitLabel->setStyleSheet(splitEnabled
                                  ? QStringLiteral("QLabel { color: #1565c0 }")
                                  : grey);

    vfoButton->setDetail(activeVfoIsB ? tr("to A") : tr("to B"));
    showStrengthInline();

    const QString idleTag = QStringLiteral("QLabel { color: #b0b0b0;"
                                           " background-color: #fbfbfb;"
                                           " border: 1px solid #dcdcdc;"
                                           " border-radius: 3px }");

    ui->aTxLabel->setText(tr("TX"));
    ui->bTxLabel->setText(tr("TX"));

    if ( !Rig::instance()->isRigConnected() )
    {
        ui->aTxLabel->setStyleSheet(idleTag);
        ui->bTxLabel->setStyleSheet(idleTag);
        return;
    }

    // in split, TX is on the VFO the rig is not receiving on
    const bool txOnB = ( splitEnabled ? !activeVfoIsB : activeVfoIsB );
    QLabel *txLabel = txOnB ? ui->bTxLabel : ui->aTxLabel;
    QLabel *rxLabel = txOnB ? ui->aTxLabel : ui->bTxLabel;

    // outlined = TX VFO, filled = transmitting now
    txLabel->setStyleSheet(keyed
        ? QStringLiteral("QLabel { color: white; background-color: #c62828;"
                         " border: 1px solid #c62828; border-radius: 3px }")
        : QStringLiteral("QLabel { color: #c62828; background-color: #fdeaea;"
                         " border: 1px solid #c62828; border-radius: 3px }"));
    rxLabel->setStyleSheet(idleTag);
}

void RigPanelWidget::splitChanged(VFOID, bool split)
{
    FCT_IDENTIFICATION;

    splitEnabled = split;
    splitButton->setActive(split);
    splitButton->setDetail(split ? tr("on") : tr("off"));

    // without rigctld/OmniRig the other VFO is only known in split
    if ( !split && meterSource != RigctldMeters && !omnirig->isOpen() )
    {
        otherFreq.clear();
        otherMode.clear();
    }

    updateVfoRows();
}

void RigPanelWidget::pttChanged(VFOID, bool ptt)
{
    FCT_IDENTIFICATION;

    keyed = ptt;
    updateVfoRows();

    // gates the recorder's voice input
    recorder->setTransmitting(ptt);

    if ( omnirig->isOpen() )
    {
        omnirigTimer->setInterval(ptt ? TX_INTERVAL : RX_INTERVAL);
        /* Wait for the pending answer and discard it; asking at once let it
           arrive later and be taken as the answer to the new request. */
        omniRigStale = ( meterAskedAt != 0 );
        transmitTurn = 0;
        sweepLeft = 0;
    }

    if ( ptt )
    {
        // no S reading on TX; do not leave a stale value up
        strengthText.clear();
        ui->sMeterBar->setValue(0);
        ui->sMeterValue->setText(QStringLiteral("--"));
        showStrengthInline();
    }

    if ( !ptt )
    {
        // otherwise the TX meters freeze at their last value
        ui->pwrBar->setValue(0);
        ui->swrBar->setValue(0);
        ui->pwrValue->setText(QStringLiteral("--"));
        ui->swrValue->setText(QStringLiteral("--"));
    }
}

/* Fully asynchronous, one request in flight over one reused connection. A
   nested event loop here let polls overlap and desynchronised CAT replies. */
void RigPanelWidget::scheduleCycle(int delayMs)
{
    FCT_IDENTIFICATION;

    cycleTimer->start(delayMs);
}

void RigPanelWidget::startCycle()
{
    FCT_IDENTIFICATION;

    if ( !extrasAvailable || !Rig::instance()->isRigConnected() )
        return;

    // no polling while the dock is hidden
    if ( !isVisible() )
    {
        scheduleCycle(RX_INTERVAL);
        return;
    }

    // the client drops a query while busy; reschedule or the loop dies
    if ( rigctld->isBusy() )
    {
        qCDebug(runtime) << "rigctld still busy, cycle deferred";
        scheduleCycle(RX_INTERVAL);
        return;
    }

    pollCounter++;
    batch.clear();
    batchLines.clear();

    // queued button commands go first
    int room = MAX_QUEUED_PER_POLL;

    while ( !pendingWrites.isEmpty() && room-- > 0 )
    {
        batch << pendingWrites.takeFirst();
        batchLines << pendingWriteLines.takeFirst();
    }

    writesInBatch = batch.size();

    if ( keyed )
    {
        batch << QStringLiteral("l RFPOWER_METER_WATTS")
              << QStringLiteral("l SWR")
              << QStringLiteral("l ALC");
        batchLines << 1 << 1 << 1;

        // supply V/A every 2nd TX poll only, to spare the CAT link
        if ( pollCounter % 2 == 0 )
        {
            batch << QStringLiteral("l VD_METER") << QStringLiteral("l ID_METER");
            batchLines << 1 << 1;
        }
    }
    else
    {
        batch << QStringLiteral("l STRENGTH");
        batchLines << 1;

        /* In split the Rig layer reports the TX frequency as current and sends
           the TX one only on change, so after A/B the rows went wrong or
           blank. Read both here: f is RX, i (get_split_freq) is TX. */
        if ( splitEnabled )
        {
            batch << QStringLiteral("f") << QStringLiteral("i");
            batchLines << 1 << 1;
        }

        if ( pollCounter % SLOW_EVERY_NTH_POLL == 0 )
        {
            switch ( ( pollCounter / SLOW_EVERY_NTH_POLL ) % SLOW_GROUPS )
            {
            case 0:
                // supply also read on RX as the reference for sag under load
                if ( otherVfoReadable && ( dualReceiver || splitEnabled ) )
                {
                    batch << QString("%1 %2").arg(QStringLiteral("\\get_vfo_info"),
                                                  otherVfoName());
                    batchLines << 5;
                }

                batch << QStringLiteral("l TEMP_METER")
                      << QStringLiteral("l VD_METER")
                      << QStringLiteral("l ID_METER");
                batchLines << 1 << 1 << 1;
                break;

            case 1:
                batch << QStringLiteral("l PREAMP")
                      << QStringLiteral("l ATT")
                      << QStringLiteral("l AGC");
                batchLines << 1 << 1 << 1;
                break;

            default:
                batch << QStringLiteral("u NB")
                      << QStringLiteral("u NR")
                      << QStringLiteral("u TUNER")
                      << QStringLiteral("s");
                batchLines << 1 << 1 << 1 << 2;
                break;
            }
        }
    }

    // rigctldResults() reschedules the cycle
    rigctld->query(batch, batchLines);
}

void RigPanelWidget::rigctldResults(const QStringList &values)
{
    FCT_IDENTIFICATION;

    ui->sourceLabel->clear();

    // walk by line counts: a command may return several values
    int at = 0;

    for ( int i = 0; i < batch.size(); i++ )
    {
        const int lines = ( i < batchLines.size() ) ? qMax(1, batchLines.at(i)) : 1;
        const QString &command = batch.at(i);
        const QString value = values.value(at);

        // PTT may have changed since sending: use readings only in their own state
        if ( command == QStringLiteral("f") && !value.isEmpty() && splitEnabled )
        {
            currentFreq = value.toDouble() / 1.0e6;
            updateVfoRows();
        }
        else if ( command == QStringLiteral("i") && !value.isEmpty() && splitEnabled )
        {
            otherFreq = formatFrequency(value.toDouble() / 1.0e6);
            updateVfoRows();
        }
        else if ( command == QStringLiteral("l STRENGTH") && !value.isEmpty() && !keyed )
            showStrength(value.toInt());
        else if ( command == QStringLiteral("l RFPOWER_METER_WATTS") && !value.isEmpty() && keyed )
            showPower(value.toDouble());
        else if ( command == QStringLiteral("l SWR") && !value.isEmpty() && keyed )
            showSwr(value.toDouble());
        else if ( command == QStringLiteral("l ALC") && !value.isEmpty() && keyed )
            ui->alcValue->setText(tr("ALC  : %1").arg(value.toDouble(), 0, 'f', 1));
        else if ( command == QStringLiteral("l TEMP_METER") && !value.isEmpty() )
            ui->tempValue->setText(tr("TEMP : %1 °C").arg(qRound(value.toDouble())));
        else if ( command == QStringLiteral("l VD_METER") && !value.isEmpty() )
        {
            supplyVolts = value.toDouble();
            haveSupply = true;
            showSupply();
        }
        else if ( command == QStringLiteral("l ID_METER") && !value.isEmpty() )
        {
            supplyAmps = value.toDouble();
            haveSupply = true;
            showSupply();
        }
        else if ( command == QStringLiteral("l PREAMP") && !value.isEmpty() )
            showControlState(OmniRigControls::Preamp, value.toInt(), preampName(value.toInt()));
        else if ( command == QStringLiteral("l ATT") && !value.isEmpty() )
            showControlState(OmniRigControls::Attenuator, value.toInt());
        else if ( command == QStringLiteral("l AGC") && !value.isEmpty() )
            showControlState(OmniRigControls::Agc, value.toInt());
        else if ( command == QStringLiteral("u NB") && !value.isEmpty() )
            showControlState(OmniRigControls::NoiseBlanker, value.toInt());
        else if ( command == QStringLiteral("u NR") && !value.isEmpty() )
            showControlState(OmniRigControls::NoiseReduction, value.toInt());
        else if ( command == QStringLiteral("u TUNER") && !value.isEmpty() )
        {
            tunerButton->setActive(value.toInt() != 0);
            tunerButton->setDetail(value.toInt() != 0 ? tr("in") : tr("out"));
        }
        else if ( command == QStringLiteral("s") && !value.isEmpty() )
        {
            // the Rig layer does not report split for this rig
            splitEnabled = ( value.toInt() != 0 );
            splitButton->setActive(splitEnabled);
            splitButton->setDetail(splitEnabled ? tr("on") : tr("off"));
            updateVfoRows();
        }
        // dropped if A/B was swapped since the request
        else if ( command.startsWith(QStringLiteral("\\get_vfo_info"))
                  && command.endsWith(otherVfoName()) )
        {
            // lines: frequency, mode, width, split, satmode
            const QString &freq = values.value(at);

            if ( !freq.isEmpty() )
            {
                otherFreq = formatFrequency(freq.toDouble() / 1.0e6);
                otherMode = values.value(at + 1);
                updateVfoRows();
            }
        }

        at += lines;
    }

    scheduleCycle(keyed ? TX_INTERVAL : RX_INTERVAL);
}

void RigPanelWidget::rigctldFailed(const QString &reason)
{
    FCT_IDENTIFICATION;

    ui->sourceLabel->setText(tr("rigctld not answering"));
    qWarning() << reason;

    // requeue the failed batch's commands; they are idempotent sets
    for ( int i = qMin(writesInBatch, batch.size()) - 1; i >= 0; i-- )
    {
        pendingWrites.prepend(batch.at(i));
        pendingWriteLines.prepend(batchLines.value(i, 1));
    }
    writesInBatch = 0;

    scheduleCycle(RX_INTERVAL * 5);
}

// per-profile key; the global key is the fallback default
QString RigPanelWidget::meterCorrectionKey(const QString &key) const
{
    FCT_IDENTIFICATION;

    if ( meterProfile.isEmpty() )
        return key;

    QString name = meterProfile;
    name.replace(QChar('/'), QChar('_')).replace(QChar('\\'), QChar('_'));

    return QStringLiteral("rigpanel/profiles/%1/%2").arg(name, key.section(QChar('/'), -1));
}

void RigPanelWidget::loadMeterCorrections()
{
    FCT_IDENTIFICATION;

    QSettings settings;
    const int cal = settings.value(meterCorrectionKey(SETTINGS_KEY_CAL),
                                   settings.value(SETTINGS_KEY_CAL, 0)).toInt();
    const double gain = settings.value(meterCorrectionKey(SETTINGS_KEY_GAIN),
                                       settings.value(SETTINGS_KEY_GAIN, 1.0)).toDouble();

    // loading must not trigger the save handlers
    QSignalBlocker blockCal(ui->calSpinBox);
    QSignalBlocker blockGain(ui->gainSpinBox);
    ui->calSpinBox->setValue(cal);
    ui->gainSpinBox->setValue(gain);
}

/* Offset shifts the whole scale (S1-S9); gain fixes the slope above S9, where
   Hamlib's table can run flatter than the rig's meter. */
void RigPanelWidget::showStrength(int db)
{
    FCT_IDENTIFICATION;

    db += ui->calSpinBox->value();

    if ( db > 0 )
        db = qRound(db * ui->gainSpinBox->value());

    strengthText = sTextFromDb(db);
    ui->sMeterBar->setValue(sBarFromDb(db));
    ui->sMeterValue->setText(strengthText);
    showStrengthInline();
}

/* VFO reads come from OmniRig's cache (no CAT traffic, no mode for the other
   VFO). Custom commands go one at a time: pending setting reads, then S meter
   on RX or TX meters in turn. Settings are swept every OMNIRIG_SWEEP_MS. */
void RigPanelWidget::pollOmniRig()
{
    FCT_IDENTIFICATION;

    if ( !isVisible() )
        return;

    qint64 freqA = 0;
    qint64 freqB = 0;

    if ( !omnirig->readVfos(freqA, freqB) )
        return;

    const qint64 other = activeVfoIsB ? freqA : freqB;
    const QString text = ( other > 0 ) ? formatFrequency(other / 1.0e6) : QString();

    if ( text != otherFreq )
    {
        otherFreq = text;
        updateVfoRows();
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if ( meterAskedAt )
    {
        if ( now - meterAskedAt < REPLY_TIMEOUT )
            return;

        // timeout: count a miss, pause the command after too many
        const int misses = omniRigMisses.value(omniRigAsked) + 1;

        if ( misses >= OMNIRIG_MAX_MISSES )
        {
            qCDebug(runtime) << "OmniRig: no answer to" << omniRigAsked.toHex() << "- resting it";
            omniRigMisses.remove(omniRigAsked);
            omniRigQuietUntil.insert(omniRigAsked, now + OMNIRIG_QUIET_MS);
        }
        else
            omniRigMisses.insert(omniRigAsked, misses);

        meterAskedAt = 0;
        omniRigStale = false;
    }

    OmniRigMeters::Request request;

    while ( !omniRigReads.isEmpty() && request.command.isEmpty() )
    {
        const OmniRigMeters::Request read = omniRigReads.takeFirst();

        if ( !omniRigSkips(read, now) )
            request = read;
    }

    if ( request.command.isEmpty() && keyed )
    {
        // next TX meter in turn that is not paused
        const QList<OmniRigMeters::Request> transmitting = OmniRigMeters::transmitRequests(omniRigInfo);

        for ( int tries = 0; tries < transmitting.size() && request.command.isEmpty(); tries++ )
        {
            const OmniRigMeters::Request next = transmitting.at(transmitTurn++ % transmitting.size());

            if ( !omniRigSkips(next, now) )
                request = next;
        }
    }
    else if ( request.command.isEmpty() )
    {
        const OmniRigMeters::Request strength = OmniRigMeters::strengthRequest(omniRigInfo);

        if ( !omniRigSkips(strength, now) )
            request = strength;
    }

    if ( !request.command.isEmpty() )
        askOmniRig(request);
}

bool RigPanelWidget::omniRigSkips(const OmniRigMeters::Request &request, qint64 now) const
{
    FCT_IDENTIFICATION;

    return request.command.isEmpty() || omniRigQuietUntil.value(request.command) > now;
}

bool RigPanelWidget::askOmniRig(const OmniRigMeters::Request &request)
{
    FCT_IDENTIFICATION;

    if ( !omnirig->sendCustomCommand(request.command, request.replyLength, request.replyEnd) )
        return false;

    omniRigAsked = request.command;
    omniRigAskedKeyed = keyed;
    meterAskedAt = QDateTime::currentMSecsSinceEpoch();
    return true;
}

/* Reads all five settings back to back (each answered in tens of ms), so
   changes made on the rig show within OMNIRIG_SWEEP_MS. */
void RigPanelWidget::startOmniRigSweep()
{
    FCT_IDENTIFICATION;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if ( !OmniRigControls::supported(omniRigInfo) || !omniRigReads.isEmpty()
         || now - lastSweepAt < OMNIRIG_SWEEP_MS )
        return;

    lastSweepAt = now;
    sweepLeft = OMNIRIG_CONTROLS;
    readNextOmniRigControl();
}

void RigPanelWidget::readNextOmniRigControl()
{
    FCT_IDENTIFICATION;

    if ( sweepLeft <= 0 || keyed || !omniRigReads.isEmpty() )
    {
        sweepLeft = 0;
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    while ( sweepLeft > 0 )
    {
        sweepLeft--;

        const OmniRigMeters::Request request = OmniRigControls::readRequest(omniRigInfo,
            static_cast<OmniRigControls::Control>(controlTurn++ % OMNIRIG_CONTROLS));

        if ( omniRigSkips(request, now) )
            continue;

        if ( !askOmniRig(request) )
            sweepLeft = 0;

        return;
    }
}

void RigPanelWidget::omniRigReply(const QByteArray &, const QByteArray &reply)
{
    FCT_IDENTIFICATION;

    OmniRigControls::State state;
    const bool control = OmniRigControls::parse(omniRigInfo, reply, state);
    // parse by the PTT state at request time (Kenwood SM0)
    const QList<OmniRigMeters::Reading> readings = control
            ? QList<OmniRigMeters::Reading>()
            : OmniRigMeters::parse(omniRigInfo, reply, omniRigAskedKeyed);

    // otherwise an ack of a write
    if ( !control && readings.isEmpty() )
        return;

    omniRigMisses.remove(omniRigAsked);
    meterAskedAt = 0;

    // asked before the PTT change
    if ( omniRigStale )
    {
        omniRigStale = false;
        return;
    }

    if ( control )
    {
        showControlState(state.control, state.value,
                         state.control == OmniRigControls::Preamp
                         ? OmniRigControls::preampName(omniRigInfo, state.value)
                         : QString());
        readNextOmniRigControl();
        return;
    }

    // show RX/TX readings only in their own state
    for ( const OmniRigMeters::Reading &reading : readings )
    {
        switch ( reading.meter )
        {
        case OmniRigMeters::Strength:
            if ( !keyed )
            {
                showStrength(qRound(reading.value));
                startOmniRigSweep();
            }
            break;

        case OmniRigMeters::Power:
            if ( keyed )
                showPower(reading.value);
            break;

        case OmniRigMeters::Swr:
            if ( keyed )
                showSwr(reading.value);
            break;

        case OmniRigMeters::Alc:
            if ( keyed )
                ui->alcValue->setText(tr("ALC  : %1").arg(reading.value, 0, 'f', 1));
            break;
        }
    }
}

// red from SWR 2
void RigPanelWidget::showSwr(double swr)
{
    FCT_IDENTIFICATION;

    ui->swrBar->setValue(qBound(0, static_cast<int>((swr - 1.0) / 2.0 * 100.0), 100));
    ui->swrValue->setText(QString::number(swr, 'f', 1));

    const bool high = ( swr >= 2.0 );

    if ( high != swrWarning )
    {
        swrWarning = high;
        ui->swrBar->setStyleSheet(high ? "QProgressBar::chunk { background-color: red }"
                                       : QString());
        ui->swrValue->setStyleSheet(high ? "QLabel { color: red; font-weight: bold }"
                                         : QString());
    }
}

/* The scale grows only past 125 % of the profile power: calibration tables
   overshoot the rating (IC-705 table ends at 12 W). One decimal below 20 W. */
void RigPanelWidget::showPower(double watts)
{
    FCT_IDENTIFICATION;

    if ( watts > maxPower * 1.25 )
    {
        const int step = ( watts <= 20.0 ) ? 5 : ( watts <= 100.0 ) ? 10 : 50;
        maxPower = qCeil(watts / step) * step;
        ui->pwrBar->setMaximum(maxPower);
        pwrScale->setTicks(powerTicks(maxPower));
    }

    ui->pwrBar->setValue(qBound(0, qRound(watts), maxPower));
    ui->pwrValue->setText(( maxPower <= 20 ) ? tr("%1 W").arg(watts, 0, 'f', 1)
                                             : tr("%1 W").arg(qRound(watts)));
}

void RigPanelWidget::showStrengthInline()
{
    FCT_IDENTIFICATION;

    QLabel *live = activeVfoIsB ? ui->bStrengthLabel : ui->aStrengthLabel;
    QLabel *idle = activeVfoIsB ? ui->aStrengthLabel : ui->bStrengthLabel;

    live->setText(strengthText);
    idle->clear();
}

void RigPanelWidget::showSupply()
{
    FCT_IDENTIFICATION;

    ui->supplyValue->setText(haveSupply
        ? tr("PA   : %1 V  %2 A").arg(supplyVolts, 0, 'f', 1)
                                 .arg(supplyAmps, 0, 'f', 1)
        : tr("PA   : -- V  -- A"));
}

// built in code: RigStateButton is local to this file, not in Designer
void RigPanelWidget::buildStateButtons()
{
    FCT_IDENTIFICATION;

    auto make = [this](RigStateButton *&target, const QString &caption,
                       const QString &tip, void (RigPanelWidget::*handler)())
    {
        target = new RigStateButton(caption, this);
        target->setToolTip(tip);
        connect(target, &QAbstractButton::clicked, this, handler);
    };

    make(preButton, tr("PRE"), tr("Preamp: IPO, AMP1, AMP2"),
         &RigPanelWidget::preampClicked);
    make(attButton, tr("ATT"), tr("Attenuator: off, 6, 12, 18 dB"),
         &RigPanelWidget::attClicked);
    make(agcButton, tr("AGC"), tr("AGC decay: fast, medium, slow, auto"),
         &RigPanelWidget::agcClicked);
    make(nbButton, tr("NB"), tr("Noise blanker"), &RigPanelWidget::nbClicked);
    make(nrButton, tr("NR"), tr("Noise reduction"), &RigPanelWidget::nrClicked);

    make(vfoButton, tr("A/B"), tr("Move the rig to the other VFO"),
         &RigPanelWidget::swapVfoClicked);
    make(splitButton, tr("SPL"), tr("Split: transmit on the other VFO"),
         &RigPanelWidget::splitClicked);
    make(tunerButton, tr("ATU"), tr("Switch the internal antenna tuner in or out"),
         &RigPanelWidget::tunerClicked);
    make(recButton, tr("REC"), recordTip(), &RigPanelWidget::recordClicked);

    recButton->setAlarm(true);
    recButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(recButton, &QWidget::customContextMenuRequested, this, &RigPanelWidget::recordMenu);

    preButton->reserveDetail(QStringLiteral("AMP2"));
    attButton->reserveDetail(QStringLiteral("18 dB"));
    agcButton->reserveDetail(QStringLiteral("SFast"));
    nbButton->reserveDetail(QStringLiteral("off"));
    nrButton->reserveDetail(QStringLiteral("off"));
    vfoButton->reserveDetail(QStringLiteral("to B"));
    splitButton->reserveDetail(QStringLiteral("off"));
    tunerButton->reserveDetail(QStringLiteral("out"));
    recButton->reserveDetail(QStringLiteral("00:00"));

    const QVector<RigStateButton *> all = { preButton, attButton, agcButton,
                                            nbButton, nrButton, vfoButton,
                                            splitButton, tunerButton, recButton };
    QSize biggest;

    for ( RigStateButton *button : all )
        biggest = biggest.expandedTo(button->sizeHint());

    for ( RigStateButton *button : all )
        button->setFixedSize(biggest);

    // fixed TX tag size; otherwise the two VFO rows' fonts make them differ
    const QSize tagSize(biggest.width(), biggest.height() * 3 / 4);

    for ( QLabel *tag : { ui->aTxLabel, ui->bTxLabel } )
        tag->setFixedSize(tagSize);

    int column = 0;

    for ( RigStateButton *button : { preButton, attButton, agcButton,
                                     nbButton, nrButton } )
        ui->buttonGrid->addWidget(button, 0, column++);

    column = 0;

    for ( RigStateButton *button : { vfoButton, splitButton, tunerButton, recButton } )
        ui->buttonGrid->addWidget(button, 1, column++);

    recButton->setDetail(tr("off"));
}

void RigPanelWidget::setRigLevel(const QString &name, int value)
{
    FCT_IDENTIFICATION;

    queueRigctldCommand(QString("L %1 %2").arg(name, QString::number(value)));
    queueRigctldCommand(QString("l %1").arg(name));
}

void RigPanelWidget::setRigFunc(const QString &name, bool on)
{
    FCT_IDENTIFICATION;

    queueRigctldCommand(QString("U %1 %2").arg(name, on ? QStringLiteral("1")
                                                        : QStringLiteral("0")));
    queueRigctldCommand(QString("u %1").arg(name));
}

// shared by the rigctld and OmniRig paths
void RigPanelWidget::showControlState(OmniRigControls::Control control, int value,
                                      const QString &preampText)
{
    switch ( control )
    {
    case OmniRigControls::Preamp:
        preampDb = value;
        preButton->setActive(value > 0);
        preButton->setDetail(preampText.isEmpty() ? tr("%1 dB").arg(value) : preampText);
        break;

    case OmniRigControls::Attenuator:
        attDb = value;
        attButton->setActive(value > 0);
        attButton->setDetail(value > 0 ? tr("%1 dB").arg(value) : tr("off"));
        break;

    case OmniRigControls::Agc:
        agcMode = value;
        agcButton->setActive(value != 0);
        agcButton->setDetail(agcName(value));
        break;

    case OmniRigControls::NoiseBlanker:
        nbButton->setActive(value != 0);
        nbButton->setDetail(value != 0 ? tr("on") : tr("off"));
        break;

    case OmniRigControls::NoiseReduction:
        nrButton->setActive(value != 0);
        nrButton->setDetail(value != 0 ? tr("on") : tr("off"));
        break;
    }
}

bool RigPanelWidget::usesOmniRigControls() const
{
    FCT_IDENTIFICATION;

    return meterSource != RigctldMeters && omnirig->isOpen()
           && OmniRigControls::supported(omniRigInfo);
}

// write now, read back before the next meter
void RigPanelWidget::stepOmniRigControl(OmniRigControls::Control control, int current)
{
    FCT_IDENTIFICATION;

    const QList<int> steps = OmniRigControls::steps(omniRigInfo, control);

    if ( steps.isEmpty() )
        return;

    const int next = steps.at(( steps.indexOf(current) + 1 ) % steps.size());
    const OmniRigMeters::Request write = OmniRigControls::writeRequest(omniRigInfo, control, next);

    if ( write.command.isEmpty()
         || !omnirig->sendCustomCommand(write.command, write.replyLength, write.replyEnd) )
        return;

    omniRigReads.prepend(OmniRigControls::readRequest(omniRigInfo, control));
}

void RigPanelWidget::preampClicked()
{
    FCT_IDENTIFICATION;

    if ( usesOmniRigControls() )
    {
        stepOmniRigControl(OmniRigControls::Preamp, preampDb);
        return;
    }

    setRigLevel(QStringLiteral("PREAMP"), nextStep(preampSteps, preampDb));
}

void RigPanelWidget::attClicked()
{
    FCT_IDENTIFICATION;

    if ( usesOmniRigControls() )
    {
        stepOmniRigControl(OmniRigControls::Attenuator, attDb);
        return;
    }

    setRigLevel(QStringLiteral("ATT"), nextStep(attSteps, attDb));
}

// off is skipped on purpose
void RigPanelWidget::agcClicked()
{
    FCT_IDENTIFICATION;

    if ( usesOmniRigControls() )
    {
        stepOmniRigControl(OmniRigControls::Agc, agcMode);
        return;
    }

    setRigLevel(QStringLiteral("AGC"), nextStep(agcSteps, agcMode));
}

// the setting after current in steps, back to the first after the last
int RigPanelWidget::nextStep(const QList<int> &steps, int current) const
{
    FCT_IDENTIFICATION;

    if ( steps.isEmpty() )
        return current;

    const int at = steps.indexOf(current);

    return steps.at(( at + 1 ) % steps.size());
}

QString RigPanelWidget::preampName(int value) const
{
    FCT_IDENTIFICATION;

    if ( value <= 0 )
        return yaesuPreampNames ? tr("IPO") : tr("off");

    const int at = preampSteps.indexOf(value);

    return at > 0 ? tr("AMP%1").arg(at) : tr("%1 dB").arg(value);
}

/* PRE, ATT and AGC steps for this model, from Hamlib's caps, as rigctld
   accepts them (the IC-705 takes preamp 1 and 2, not dB). */
void RigPanelWidget::loadRigctldSteps(int model)
{
    FCT_IDENTIFICATION;

    // FTDX101, also used when Hamlib has no caps for the model
    preampSteps = { 0, 10, 20 };
    attSteps = { 0, 6, 12, 18 };
    agcSteps = { 2, 5, 3, 6 };
    yaesuPreampNames = true;
    otherVfoReadable = true;
    dualReceiver = true;

    const struct rig_caps *caps = rig_get_caps(model);

    if ( !caps )
        return;

    const QString maker = QString::fromLatin1(caps->mfg_name);

    yaesuPreampNames = ( maker == QStringLiteral("Yaesu") );

    /* On an Icom, get_vfo_info for the other VFO switches rigctld's shared
       VFO state for a moment: QLog's own driver then reads VFO B's frequency
       or garbage (IC-705, Hamlib 4.7.2), so the other VFO is not read. */
    otherVfoReadable = ( maker != QStringLiteral("Icom") );

    // Main and Sub in the VFO list mean a second receiver (FTDX101, IC-7610)
    int vfos = 0;

    for ( int i = 0; i < HAMLIB_FRQRANGESIZ && !RIG_IS_FRNG_END(caps->rx_range_list1[i]); i++ )
        vfos |= caps->rx_range_list1[i].vfo;

    dualReceiver = ( vfos & ( RIG_VFO_MAIN | RIG_VFO_SUB ) ) != 0;

    QList<int> preamp = { 0 };
    QList<int> att = { 0 };

    for ( int i = 0; i < HAMLIB_MAXDBLSTSIZ && caps->preamp[i] != 0; i++ )
        preamp << caps->preamp[i];

    for ( int i = 0; i < HAMLIB_MAXDBLSTSIZ && caps->attenuator[i] != 0; i++ )
        att << caps->attenuator[i];

    preampSteps = preamp;
    attSteps = att;

#if HAMLIB_VERSION >= HAMLIB_VERSION_CHECK(4, 6, 0)
    /* The whole table, not agc_level_count: the IC-705 and IC-7300 caps
       list OFF, FAST, MEDIUM, SLOW with a count of 3, which drops SLOW.
       Unused entries are 0 (OFF) and skipped with it. */
    QList<int> agc;

    if ( caps->agc_level_count > 0 )
    {
        for ( int i = 0; i < HAMLIB_MAX_AGC_LEVELS; i++ )
        {
            const int level = static_cast<int>(caps->agc_levels[i]);

            if ( level != RIG_AGC_OFF && !agc.contains(level) )
                agc << level;
        }
    }

    if ( !agc.isEmpty() )
        agcSteps = agc;
#endif

    auto names = [](const QList<int> &steps) -> QString
    {
        QStringList out;

        for ( int step : steps )
            out << QString::number(step);

        return out.join(QStringLiteral(", "));
    };

    preButton->setToolTip(tr("Preamp: %1").arg(names(preampSteps)));
    attButton->setToolTip(tr("Attenuator: %1 dB").arg(names(attSteps)));

    QStringList agcNames;

    for ( int step : static_cast<const QList<int> &>(agcSteps) )
        agcNames << agcName(step);

    agcButton->setToolTip(tr("AGC: %1").arg(agcNames.join(QStringLiteral(", "))));

    qCDebug(runtime) << "steps for model" << model << "preamp" << preampSteps
                     << "att" << attSteps << "agc" << agcSteps;
}

void RigPanelWidget::nbClicked()
{
    FCT_IDENTIFICATION;

    if ( usesOmniRigControls() )
        stepOmniRigControl(OmniRigControls::NoiseBlanker, nbButton->isActive() ? 1 : 0);
    else
        setRigFunc(QStringLiteral("NB"), !nbButton->isActive());
}

void RigPanelWidget::nrClicked()
{
    FCT_IDENTIFICATION;

    if ( usesOmniRigControls() )
        stepOmniRigControl(OmniRigControls::NoiseReduction, nrButton->isActive() ? 1 : 0);
    else
        setRigFunc(QStringLiteral("NR"), !nrButton->isActive());
}

void RigPanelWidget::clearRigState()
{
    FCT_IDENTIFICATION;

    supplyVolts = supplyAmps = 0.0;
    haveSupply = false;
    showSupply();
    ui->alcValue->setText(tr("ALC  : --"));
    ui->tempValue->setText(tr("TEMP : --"));

    preampDb = attDb = agcMode = 0;

    for ( RigStateButton *button : { preButton, attButton, agcButton, nbButton,
                                     nrButton, tunerButton, splitButton } )
    {
        button->setActive(false);
        button->setDetail(QString());
    }
}

// keeps one request in flight; costs up to one poll interval of latency
void RigPanelWidget::queueRigctldCommand(const QString &command, int lines)
{
    FCT_IDENTIFICATION;

    if ( meterSource != RigctldMeters )
    {
        qCWarning(runtime) << "no rigctld to send" << command << "to";
        return;
    }

    pendingWrites << command;
    pendingWriteLines << qMax(1, lines);
}

// the queue still sends at most MAX_QUEUED_PER_POLL per pass
void RigPanelWidget::primeSlowReadings()
{
    FCT_IDENTIFICATION;

    if ( otherVfoReadable && dualReceiver )
        queueRigctldCommand(QString("%1 %2").arg(QStringLiteral("\\get_vfo_info"),
                                                 otherVfoName()), 5);

    queueRigctldCommand(QStringLiteral("s"), 2);

    for ( const QString &command : { QStringLiteral("l TEMP_METER"),
                                     QStringLiteral("l VD_METER"),
                                     QStringLiteral("l ID_METER"),
                                     QStringLiteral("l PREAMP"),
                                     QStringLiteral("l ATT"),
                                     QStringLiteral("l AGC"),
                                     QStringLiteral("u NB"),
                                     QStringLiteral("u NR"),
                                     QStringLiteral("u TUNER") } )
        queueRigctldCommand(command);
}

void RigPanelWidget::swapVfoClicked()
{
    FCT_IDENTIFICATION;

    // explicit target VFO, not a toggle, so a retried command is harmless
    const QString target = otherVfoName();
    const QString left = activeVfoIsB ? QStringLiteral("VFOB") : QStringLiteral("VFOA");
    const QString leftFreq = currentFreq > 0.0 ? formatFrequency(currentFreq) : QString();
    const bool swapSplit = splitEnabled && !dualReceiver;

    queueRigctldCommand(QStringLiteral("V ") + target);

    /* A single-receiver rig in split now transmits on the VFO it left, but
       Hamlib keeps the old TX VFO and get_split_freq would read the RX one. */
    if ( swapSplit )
    {
        queueRigctldCommand(QStringLiteral("S 1 ") + left);
        queueRigctldCommand(QStringLiteral("s"), 2);
    }

    // the IC-705 through Hamlib does not report its VFO, so follow it here
    if ( !rigReportsVfo )
        setActiveVfo(target == QStringLiteral("VFOB"));

    // until the next read, the TX row holds what was received on
    if ( swapSplit )
    {
        otherFreq = leftFreq;
        updateVfoRows();
    }
}

/* Rig::setSplit needs native get_split_vfo and get_split_freq; the FTdx101
   backend emulates the latter, so it does nothing there. Use rigctld directly
   when available, the Rig layer otherwise. */
void RigPanelWidget::splitClicked()
{
    FCT_IDENTIFICATION;

    const bool wanted = !splitEnabled;

    if ( meterSource != RigctldMeters )
    {
        Rig::instance()->setSplit(wanted);
        return;
    }

    const QString txVfo = wanted ? otherVfoName()
                                 : ( activeVfoIsB ? QStringLiteral("VFOB")
                                                  : QStringLiteral("VFOA") );

    queueRigctldCommand(QString("S %1 %2").arg(wanted ? 1 : 0).arg(txVfo));
    // answers: split flag, TX VFO
    queueRigctldCommand(QStringLiteral("s"), 2);
}

void RigPanelWidget::tunerClicked()
{
    FCT_IDENTIFICATION;
    setRigFunc(QStringLiteral("TUNER"), !tunerButton->isActive());
}

void RigPanelWidget::registerContactWidget(const NewContactWidget *widget)
{
    FCT_IDENTIFICATION;
    contact = widget;
}

QString RigPanelWidget::recordTip() const
{
    FCT_IDENTIFICATION;

    return tr("Record the contact. The recording starts from the seconds before "
              "the press, so the start of a contact is kept. Your own voice comes "
              "through the rig when its transmit monitor (MONI) is on, or from a "
              "separate input. Right-click for where it records from and saves to.");
}

void RigPanelWidget::applyRecordingSettings()
{
    FCT_IDENTIFICATION;

    if ( recorder->isRecording() )
        return;

    recorder->stopListening();
    recorder->setDevice(RigRecordingDialog::device());
    recorder->setVoiceDevice(RigRecordingDialog::voiceDevice());
    recorder->setStereo(RigRecordingDialog::stereo());
    recorder->setFolder(RigRecordingDialog::folder());
    recorder->setRate(RigRecordingDialog::rate());
    recorder->setBufferSeconds(RigRecordingDialog::bufferSeconds());

    // no device yet: the first REC press opens the dialog
    if ( !RigRecordingDialog::device().isEmpty() && recorder->listen() )
    {
        recButton->setDetail(tr("off"));
        recButton->setToolTip(recordTip());
    }
}

// CALL_band_mode, unknown parts left out
QString RigPanelWidget::recordingLabel() const
{
    FCT_IDENTIFICATION;

    QStringList parts;

    if ( contact && !contact->getCallsign().trimmed().isEmpty() )
        parts << contact->getCallsign().trimmed().toUpper();

    if ( currentFreq > 0.0 )
    {
        const QString band = BandPlan::freq2Band(currentFreq).name;

        if ( !band.isEmpty() )
            parts << band;
    }

    if ( !logMode.isEmpty() )
        parts << logMode;

    return parts.join(QStringLiteral("_"));
}

void RigPanelWidget::recordClicked()
{
    FCT_IDENTIFICATION;

    if ( recorder->isRecording() )
    {
        recorder->stop(recordingLabel());
        return;
    }

    if ( RigRecordingDialog::device().isEmpty() )
    {
        RigRecordingDialog dialog(this);

        if ( dialog.exec() != QDialog::Accepted )
            return;

        applyRecordingSettings();
    }

    recorder->start();
}

void RigPanelWidget::recordMenu(const QPoint &where)
{
    FCT_IDENTIFICATION;

    QMenu menu(this);
    QAction *settings = menu.addAction(tr("Recording settings..."));
    QAction *open = menu.addAction(tr("Open the recordings folder"));

    QAction *chosen = menu.exec(recButton->mapToGlobal(where));

    if ( chosen == settings )
    {
        RigRecordingDialog dialog(this);

        if ( dialog.exec() == QDialog::Accepted )
            applyRecordingSettings();
    }
    else if ( chosen == open )
    {
        QDir().mkpath(recorder->folder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(recorder->folder()));
    }
}

void RigPanelWidget::recordingChanged(bool on)
{
    FCT_IDENTIFICATION;

    recButton->setActive(on);

    if ( on )
    {
        showRecordingTime();
        recordTimer->start();
    }
    else
    {
        recordTimer->stop();
        recButton->setDetail(tr("off"));
    }
}

// file length including the pre-roll
void RigPanelWidget::showRecordingTime()
{
    FCT_IDENTIFICATION;

    const int seconds = static_cast<int>(recorder->recordedSeconds());

    recButton->setDetail(QString::asprintf("%02d:%02d", seconds / 60, seconds % 60));
}
