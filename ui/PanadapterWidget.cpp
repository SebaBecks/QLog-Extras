#include <cmath>
#include <algorithm>

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "PanadapterWidget.h"
#include "PanadapterView.h"
#include "sdr/SdrDevice.h"
#include "sdr/IqSpectrum.h"
#include "sdr/RigScopeSource.h"
#include "rig/Rig.h"
#include "data/RigProfile.h"
#include "data/BandPlan.h"
#include "data/Data.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.ui.panadapterwidget");

#define SETTINGS_SPAN        "panadapter/span"
#define SETTINGS_RUNNING     "panadapter/running"
#define SETTINGS_RANGE       "panadapter/range"
#define SETTINGS_LEVEL       "panadapter/level"

#define MIN_FFT              4096
#define MAX_FFT              262144
#define SPECTRA_PER_SECOND   10.0
/* Band edges roll off in the receiver's filter. */
#define USABLE_SHARE         0.9
/* 2 bins per pixel so narrow signals do not fall between pixels. */
#define BINS_PER_PIXEL       2.0
#define MAX_AVERAGE          16
/* Rig polled 1-2x per second reports a new frequency within this. */
#define TUNE_SETTLE_MS       1500
/* Same callsign within this distance replaces the older spot. */
#define SPOT_SAME_HZ         5000.0
#define SPOT_REFRESH_MS      30000

/* IARU Region 1 TX allocations in kHz; national licences may be narrower. */
static const struct { double low; double high; } region1[] = {
    { 135.7, 137.8 }, { 472.0, 479.0 }, { 1810.0, 2000.0 }, { 3500.0, 3800.0 },
    { 5351.5, 5366.5 }, { 7000.0, 7200.0 }, { 10100.0, 10150.0 }, { 14000.0, 14350.0 },
    { 18068.0, 18168.0 }, { 21000.0, 21450.0 }, { 24890.0, 24990.0 }, { 28000.0, 29700.0 },
    { 50000.0, 52000.0 }, { 70000.0, 70500.0 }, { 144000.0, 146000.0 }, { 430000.0, 440000.0 }
};

/* Half spans in Hz; 0 = full receiver bandwidth, SPAN_BAND = whole band. */
#define SPAN_BAND            -1
static const int spans[] = { 5000, 10000, 25000, 50000, 100000, 250000, 500000, 0, SPAN_BAND };
#define BAND_MARGIN          0.04
/* "Band" span outside amateur bands. */
#define OUTSIDE_BAND_HALF    100000.0

PanadapterWidget::PanadapterWidget(QWidget *parent) :
    QWidget(parent),
    runButton(new QToolButton(this)),
    spanCombo(new QComboBox(this)),
    rangeSlider(new QSlider(Qt::Horizontal, this)),
    levelSlider(new QSlider(Qt::Horizontal, this)),
    statusLabel(new QLabel(this)),
    settingsButton(new QToolButton(this)),
    view(new PanadapterView(this))
{
    FCT_IDENTIFICATION;

    config = PanadapterConfig::load();

    runButton->setText(tr("Start"));
    runButton->setCheckable(true);
    runButton->setToolTip(tr("Start or stop the receiver"));

    for ( int half : spans )
        spanCombo->addItem(half == SPAN_BAND ? tr("Band")
                           : half ? tr("±%1 kHz").arg(half / 1000) : tr("Full"), half);
    spanCombo->setToolTip(tr("How much of the band the picture shows"));

    QSettings settings;

    rangeSlider->setRange(10, 60);
    rangeSlider->setValue(settings.value(SETTINGS_RANGE, 30).toInt());
    rangeSlider->setFixedWidth(90);
    rangeSlider->setToolTip(tr("Contrast: how many decibels the colours span. "
                               "Less makes weak stations stand out."));
    levelSlider->setRange(-20, 30);
    levelSlider->setValue(settings.value(SETTINGS_LEVEL, 0).toInt());
    levelSlider->setFixedWidth(90);
    levelSlider->setToolTip(tr("Level: where the colours start against the noise. "
                               "Lower shows more of the noise and the weak stations in it."));

    settingsButton->setText(tr("Settings..."));
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QHBoxLayout *bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 0, 0);
    bar->addWidget(runButton);
    bar->addWidget(spanCombo);
    bar->addSpacing(8);
    bar->addWidget(new QLabel(tr("Contrast"), this));
    bar->addWidget(rangeSlider);
    bar->addWidget(new QLabel(tr("Level"), this));
    bar->addWidget(levelSlider);
    bar->addSpacing(8);
    bar->addWidget(statusLabel, 1);
    bar->addWidget(settingsButton);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    layout->addLayout(bar);
    layout->addWidget(view, 1);

    levelsChanged();
    showBands();

    spectrum = new IqSpectrum;
    spectrum->moveToThread(&worker);
    worker.setObjectName(QStringLiteral("panadapter"));
    worker.start();
    connect(spectrum, &IqSpectrum::spectrumReady, this, &PanadapterWidget::spectrumArrived);

    spanCombo->setCurrentIndex(qBound(0, settings.value(SETTINGS_SPAN, 4).toInt(), spanCombo->count() - 1));

    connect(runButton, &QToolButton::toggled, this, &PanadapterWidget::runToggled);
    connect(spanCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &PanadapterWidget::spanChosen);
    connect(settingsButton, &QToolButton::clicked, this, &PanadapterWidget::openSettings);
    connect(rangeSlider, &QSlider::valueChanged, this, &PanadapterWidget::levelsChanged);
    connect(levelSlider, &QSlider::valueChanged, this, &PanadapterWidget::levelsChanged);
    connect(view, &PanadapterView::clicked, this, &PanadapterWidget::viewClicked);
    connect(view, &PanadapterView::doubleClicked, this, &PanadapterWidget::recentre);
    connect(view, &PanadapterView::wheelTurned, this, &PanadapterWidget::wheelTurned);
    connect(view, &PanadapterView::spotClicked, this, &PanadapterWidget::spotClicked);
    connect(view, &PanadapterView::dragged, this, &PanadapterWidget::viewDragged);

    QTimer *spotTimer = new QTimer(this);
    connect(spotTimer, &QTimer::timeout, this, &PanadapterWidget::showSpots);
    spotTimer->start(SPOT_REFRESH_MS);

    settleTimer = new QTimer(this);
    settleTimer->setSingleShot(true);
    connect(settleTimer, &QTimer::timeout, this, &PanadapterWidget::settleTuning);

    connect(Rig::instance(), &Rig::frequencyChanged, this, &PanadapterWidget::updateFrequency);
    connect(Rig::instance(), &Rig::modeChanged, this, &PanadapterWidget::updateMode);
    connect(Rig::instance(), &Rig::pttChanged, this, &PanadapterWidget::updatePTT);

    showStatus();
    updateWindow();
    view->setMessage(tr("Press Start to see the band"));

    if ( settings.value(SETTINGS_RUNNING, false).toBool() )
        QTimer::singleShot(0, this, [this]() { runButton->setChecked(true); });
}

PanadapterWidget::~PanadapterWidget()
{
    FCT_IDENTIFICATION;

    stopReceiver();
    worker.quit();
    worker.wait();
    delete spectrum;
}

void PanadapterWidget::runToggled(bool on)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << on;

    if ( on )
    {
        if ( !startReceiver() )
        {
            QSignalBlocker blocker(runButton);
            runButton->setChecked(false);
        }
    }
    else
        stopReceiver();

    runButton->setText(runButton->isChecked() ? tr("Stop") : tr("Start"));
    QSettings().setValue(SETTINGS_RUNNING, runButton->isChecked());
}

bool PanadapterWidget::startReceiver()
{
    FCT_IDENTIFICATION;

    stopReceiver();

    if ( config.connection == PanadapterConfig::RigScope )
        return startRigScope();

    device = SdrDevice::create(config.driver, this);
    if ( !device )
    {
        view->setMessage(tr("Unknown receiver driver: %1").arg(config.driver));
        return false;
    }

    device->setLibraryPath(config.library);

    if ( !device->open(config.device) )
    {
        view->setMessage(device->lastError());
        delete device;
        device = nullptr;
        return false;
    }

    receiverCenter = config.connection == PanadapterConfig::IfOutput
                   ? config.ifCenter
                   : ( rx > 0 ? rx + passbandOffset() : 7100000.0 );

    bool ok = device->setDirectSampling(config.directSampling)
              && device->setSampleRate(config.sampleRate)
              && device->setCenterFrequency(receiverCenter)
              && device->setGain(config.gain);

    if ( ok && config.connection == PanadapterConfig::Antenna )
        ok = device->setFrequencyCorrection(config.ppm);

    if ( ok && config.connection == PanadapterConfig::Antenna && config.biasTee )
        ok = device->setBiasTee(true);

    if ( !ok )
    {
        view->setMessage(device->lastError());
        delete device;
        device = nullptr;
        return false;
    }

    connect(device, &SdrDevice::samplesReady, spectrum, &IqSpectrum::process);
    connect(device, &SdrDevice::failed, this, &PanadapterWidget::receiverFailed);

    configureSpectrum();

    if ( !device->start() )
    {
        view->setMessage(device->lastError());
        delete device;
        device = nullptr;
        return false;
    }

    view->setMessage(QString());
    view->clear();
    showStatus();
    updateWindow();
    return true;
}

/* Needs a Hamlib rig profile with shared rigctld; it owns the CAT port. */
bool PanadapterWidget::startRigScope()
{
    FCT_IDENTIFICATION;

    const RigProfile profile = RigProfilesManager::instance()->getCurProfile1();

    if ( profile.driver != Rig::HAMLIB_DRIVER || !profile.shareRigctld )
    {
        view->setMessage(tr("The rig's scope comes through rigctld - choose a Hamlib rig "
                            "profile with Share Rig via port"));
        return false;
    }

    scope = new RigScopeSource(this);

    if ( !scope->start(profile.rigctldPort, config.scopeGroup, quint16(config.scopePort)) )
    {
        view->setMessage(scope->lastError());
        delete scope;
        scope = nullptr;
        return false;
    }

    connect(scope, &RigScopeSource::lineReady, this, &PanadapterWidget::scopeLineArrived);
    connect(scope, &RigScopeSource::statusChanged, this, [this](const QString &text)
    {
        view->setMessage(text);
    });

    scopeLow = scopeHigh = 0.0;
    view->clear();
    showStatus();
    updateWindow();
    view->setMessage(tr("Waiting for the rig's scope..."));
    return true;
}

void PanadapterWidget::stopReceiver()
{
    FCT_IDENTIFICATION;

    if ( scope )
    {
        scope->disconnect(this);
        scope->stop();
        delete scope;
        scope = nullptr;
        scopeLow = scopeHigh = 0.0;
    }

    if ( device )
    {
        device->disconnect(this);
        device->close();
        delete device;
        device = nullptr;
    }

    showStatus();
}

bool PanadapterWidget::running() const
{
    FCT_IDENTIFICATION;

    return device || scope;
}

double PanadapterWidget::widestHalf() const
{
    FCT_IDENTIFICATION;

    // IC-705 default span until the first sweep
    if ( config.connection == PanadapterConfig::RigScope )
        return scopeHigh > scopeLow ? ( scopeHigh - scopeLow ) / 2.0 : 25000.0;

    const int rate = device ? device->sampleRate() : config.sampleRate;
    return rate * USABLE_SHARE / 2.0;
}

void PanadapterWidget::receiverFailed(const QString &reason)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << reason;

    stopReceiver();
    view->setMessage(reason);
    QSignalBlocker blocker(runButton);
    runButton->setChecked(false);
    runButton->setText(tr("Start"));
}

void PanadapterWidget::openSettings()
{
    FCT_IDENTIFICATION;

    PanadapterSettingsDialog dialog(this);

    if ( dialog.exec() != QDialog::Accepted )
        return;

    config = PanadapterConfig::load();
    pan = clampPan(pan);
    showSpots();

    if ( running() )
        startReceiver();
    else
    {
        showStatus();
        updateWindow();
    }
}

void PanadapterWidget::spanChosen(int index)
{
    FCT_IDENTIFICATION;

    QSettings().setValue(SETTINGS_SPAN, index);
    pan = clampPan(pan);
    bandPan = 0.0;
    lastBandLow = 0.0;
    configureSpectrum();
    updateWindow();
}

double PanadapterWidget::halfSpan() const
{
    FCT_IDENTIFICATION;

    const double widest = widestHalf();
    const int chosen = spanCombo->currentData().toInt();

    if ( chosen == SPAN_BAND )
    {
        double lowHz, highHz;
        if ( !bandAround(rx + passbandOffset(), lowHz, highHz) )
            return qMin(OUTSIDE_BAND_HALF, widest);
        /* May exceed the receiver bandwidth; the rest stays empty. */
        return ( highHz - lowHz ) / 2.0 * ( 1.0 + 2.0 * BAND_MARGIN );
    }
    return chosen > 0 ? qMin<double>(chosen, widest) : widest;
}

bool PanadapterWidget::showingBand() const
{
    FCT_IDENTIFICATION;

    return spanCombo->currentData().toInt() == SPAN_BAND;
}

bool PanadapterWidget::bandAround(double hz, double &lowHz, double &highHz) const
{
    FCT_IDENTIFICATION;

    for ( const auto &band : region1 )
        if ( hz >= band.low * 1000.0 && hz <= band.high * 1000.0 )
        {
            lowHz = band.low * 1000.0;
            highHz = band.high * 1000.0;
            return true;
        }
    return false;
}

void PanadapterWidget::configureSpectrum()
{
    FCT_IDENTIFICATION;

    if ( !device )
        return;

    /* FFT size for BINS_PER_PIXEL in the visible window. */
    const int rate = device->sampleRate();
    const double wanted = BINS_PER_PIXEL * rate * qMax(200, view->pixels()) / ( 2.0 * halfSpan() );
    int size = MIN_FFT;
    while ( size < wanted && size < MAX_FFT )
        size <<= 1;
    /* Average all samples; smoother noise makes weak signals visible. */
    const int average = qBound(1, int(rate / SPECTRA_PER_SECOND / size), MAX_AVERAGE);

    QMetaObject::invokeMethod(spectrum, [this, rate, size, average]()
    {
        spectrum->setSize(size);
        spectrum->setAverage(average);
        spectrum->setRate(SPECTRA_PER_SECOND);
        spectrum->setSampleRate(rate);
    }, Qt::QueuedConnection);
}

double PanadapterWidget::passbandOffset() const
{
    FCT_IDENTIFICATION;

    const QString mode = rawMode.toUpper();

    /* Data sideband: Hamlib PKTUSB, OmniRig DIG_U, flrig DATA-U. */
    if ( mode.contains(QLatin1String("LSB")) || mode == QLatin1String("DIG_L")
         || mode == QLatin1String("DATA-L") )
        return -config.sidebandOffset;
    if ( mode.contains(QLatin1String("USB")) || mode == QLatin1String("DIG_U")
         || mode == QLatin1String("DATA-U") )
        return config.sidebandOffset;
    if ( mode.startsWith(QLatin1String("CWR")) || mode == QLatin1String("CW-R") )
        return -config.cwOffset;
    if ( mode.startsWith(QLatin1String("CW")) )
        return config.cwOffset;
    return 0.0;
}

double PanadapterWidget::passbandWidth() const
{
    FCT_IDENTIFICATION;

    if ( bandwidth > 0 )
        return bandwidth;

    const QString mode = rawMode.toUpper();

    if ( mode.startsWith(QLatin1String("CW")) )
        return 500.0;
    if ( mode.startsWith(QLatin1String("AM")) )
        return 6000.0;
    if ( mode.contains(QLatin1String("FM")) )
        return 12000.0;
    return 2700.0;
}

void PanadapterWidget::updateWindow()
{
    FCT_IDENTIFICATION;

    const double half = halfSpan();

    if ( rx <= 0 )
    {
        /* No rig frequency: show the receiver's own scale (the IF). */
        const double centre = ( config.connection == PanadapterConfig::IfOutput ? config.ifCenter : receiverCenter ) + pan;
        view->setWindow(centre - half, centre + half);
        view->setTuning(0, 0, 0);
        if ( running() )
            view->setMessage(tr("No frequency from the rig - connect it in QLog to see the band"));
        return;
    }

    const double middle = rx + passbandOffset();
    const double width = passbandWidth();
    double centre = middle + pan;
    double bandLow, bandHigh;

    if ( showingBand() && bandAround(middle, bandLow, bandHigh) )
    {
        if ( bandLow != lastBandLow )
        {
            bandPan = 0.0;
            lastBandLow = bandLow;
            configureSpectrum();
        }
        centre = ( bandLow + bandHigh ) / 2.0 + bandPan;
    }

    if ( clickedOnCarrier() )
        view->setPointerPassband(passbandOffset() - width / 2, passbandOffset() + width / 2);
    else
        view->setPointerPassband(-width / 2, width / 2);

    if ( device && config.connection == PanadapterConfig::Antenna )
    {
        /* Retune only when the window leaves the usable bandwidth. */
        const double reach = device->sampleRate() * USABLE_SHARE / 2.0;
        if ( std::fabs(centre - receiverCenter) + half > reach )
        {
            receiverCenter = std::round(centre / 1000.0) * 1000.0;
            device->setCenterFrequency(receiverCenter);
        }
    }

    view->setWindow(centre - half, centre + half);
    view->setTuning(rx, middle - width / 2, middle + width / 2);
    /* Keep rig scope status messages across retunes. */
    if ( device || ( scope && scopeHigh > scopeLow && !scope->isSilent() ) )
        view->setMessage(QString());
}

/* Sweep is in on-air Hz; its span sets the widest view. */
void PanadapterWidget::scopeLineArrived(const QVector<float> &decibels, double firstHz, double binHz)
{
    FCT_IDENTIFICATION;

    if ( !scope || decibels.isEmpty() )
        return;

    const double low = firstHz - binHz / 2.0;
    const double high = low + binHz * decibels.size();
    const bool first = scopeHigh <= scopeLow;
    const bool resized = !first && std::fabs(( high - low ) - ( scopeHigh - scopeLow )) > 1.0;

    scopeLow = low;
    scopeHigh = high;

    if ( first || resized )
    {
        pan = clampPan(pan);
        updateWindow();
        showStatus();
    }

    view->addSpectrum(decibels, firstHz, binHz);
}

void PanadapterWidget::spectrumArrived(const QVector<float> &decibels)
{
    FCT_IDENTIFICATION;

    if ( !device || decibels.isEmpty() )
        return;

    const int bins = decibels.size();
    const double binHz = double(device->sampleRate()) / bins;
    const double lowest = receiverCenter - ( bins / 2 ) * binHz;

    if ( config.connection == PanadapterConfig::Antenna || rx <= 0 )
    {
        view->addSpectrum(decibels, lowest, binHz);
        return;
    }

    /* IF centre = passband centre; mirrored if inverted (FTdx101 MAIN 9.005 MHz). */
    const double middle = rx + passbandOffset();

    if ( !config.inverted )
    {
        view->addSpectrum(decibels, middle + ( lowest - config.ifCenter ), binHz);
        return;
    }

    QVector<float> turned(bins);
    std::reverse_copy(decibels.cbegin(), decibels.cend(), turned.begin());
    const double highest = lowest + ( bins - 1 ) * binHz;
    view->addSpectrum(turned, middle - ( highest - config.ifCenter ), binHz);
}

void PanadapterWidget::viewClicked(double hz)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << hz;

    if ( rx <= 0 || vfo <= 0 )
        return;

    /* Shift the VFO by the RX delta to keep RIT unchanged. */
    const double wantedRx = clickedOnCarrier() ? hz : hz - passbandOffset();
    const double newVfo = std::round(( vfo + ( wantedRx - rx ) ) / 10.0) * 10.0;

    tuneTo(newVfo, pan != 0.0);
}

bool PanadapterWidget::clickedOnCarrier() const
{
    FCT_IDENTIFICATION;

    const QString mode = rawMode.toUpper();

    if ( mode.contains(QLatin1String("PKT")) || mode.contains(QLatin1String("DATA"))
         || mode.startsWith(QLatin1String("DIG"))
         || mode.startsWith(QLatin1String("CW")) || mode.startsWith(QLatin1String("RTTY")) )
        return false;
    return true;
}

double PanadapterWidget::wheelStep() const
{
    FCT_IDENTIFICATION;

    return clickedOnCarrier() ? 100.0 : 10.0;
}

void PanadapterWidget::tuneTo(double newVfo, bool keepPicture)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << newVfo << keepPicture;

    if ( newVfo <= 0 || vfo <= 0 )
        return;

    moveMarks(newVfo, keepPicture);
    Rig::instance()->setFrequency(newVfo);
}

void PanadapterWidget::moveMarks(double newVfo, bool keepPicture)
{
    FCT_IDENTIFICATION;

    if ( newVfo <= 0 || vfo <= 0 )
        return;

    const double moved = newVfo - vfo;

    if ( keepPicture )
    {
        pan -= moved;
        /* Recentre before the mark leaves the window. */
        if ( std::fabs(pan) > halfSpan() * 0.9 )
            pan = 0.0;
        pan = clampPan(pan);
    }

    tuneTarget = newVfo;
    tuneSentAt = QDateTime::currentMSecsSinceEpoch();
    // held report belongs to the previous tune
    heldVfo = 0.0;
    settleTimer->stop();
    vfo = newVfo;
    rx += moved;
    updateWindow();
}

void PanadapterWidget::wheelTurned(int steps, Qt::KeyboardModifiers modifiers)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << steps << modifiers;

    if ( modifiers & Qt::ControlModifier )
    {
        /* Up zooms in (narrow spans first). From "Band", continue from the
           nearest fixed span. */
        const int fixed = spanCombo->count() - 2;
        int index = spanCombo->currentIndex();
        if ( showingBand() )
        {
            index = 0;
            while ( index < fixed && spanCombo->itemData(index).toInt() > 0
                    && spanCombo->itemData(index).toInt() < halfSpan() )
                index++;
        }
        spanCombo->setCurrentIndex(qBound(0, index - steps, fixed));
        return;
    }

    if ( modifiers & Qt::ShiftModifier )
    {
        /* Pan the view, not the rig: 1/10 span per notch. */
        pan = clampPan(pan + steps * halfSpan() / 5.0);
        updateWindow();
        return;
    }

    if ( vfo <= 0 )
        return;

    /* Snap to the step grid first, then one step per notch. */
    const bool onTheWay = tuneTarget > 0
                          && QDateTime::currentMSecsSinceEpoch() - tuneSentAt < TUNE_SETTLE_MS;
    const double from = onTheWay ? tuneTarget : vfo;
    const double step = wheelStep();
    const double grid = steps > 0 ? std::floor(from / step + 1e-6) : std::ceil(from / step - 1e-6);
    tuneTo(( grid + steps ) * step, true);
}

void PanadapterWidget::viewDragged(double hz)
{
    FCT_IDENTIFICATION;

    if ( showingBand() )
        bandPan += hz;
    else
        pan = clampPan(pan + hz);
    updateWindow();
}

void PanadapterWidget::recentre()
{
    FCT_IDENTIFICATION;

    pan = 0.0;
    bandPan = 0.0;
    updateWindow();
}

double PanadapterWidget::clampPan(double wanted) const
{
    FCT_IDENTIFICATION;

    if ( config.connection == PanadapterConfig::Antenna )
        return wanted;

    // IF output and rig scope: limited to the source bandwidth
    const double room = qMax(0.0, widestHalf() - halfSpan());
    return qBound(-room, wanted, room);
}

void PanadapterWidget::addSpot(const DxSpot &spot)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << spot.callsign << spot.freq;

    if ( config.spotMinutes <= 0 || spot.freq <= 0 )
        return;

    for ( int i = spots.size() - 1; i >= 0; i-- )
        if ( spots[i].callsign.compare(spot.callsign, Qt::CaseInsensitive) == 0
             && std::fabs(spots[i].freq - spot.freq) * 1e6 < SPOT_SAME_HZ )
            spots.removeAt(i);

    spots.append(spot);
    showSpots();
}

void PanadapterWidget::showSpots()
{
    FCT_IDENTIFICATION;

    const QDateTime oldest = QDateTime::currentDateTimeUtc().addSecs(-60 * qMax(0, config.spotMinutes));

    for ( int i = spots.size() - 1; i >= 0; i-- )
        if ( config.spotMinutes <= 0 || spots[i].dateTime < oldest )
            spots.removeAt(i);

    QVector<PanadapterView::SpotMark> marks;
    shownSpots.clear();

    for ( const DxSpot &spot : static_cast<const QList<DxSpot> &>(spots) )
    {
        const double hz = spot.freq * 1e6;
        const QColor colour = Data::statusToColor(spot.status, spot.dupeCount, QColor(225, 228, 240));
        const QString tip = QStringLiteral("<b>%1</b> de %2<br/>%3 kHz, %4<br/>%5<br/>%6 UTC")
                                .arg(spot.callsign.toHtmlEscaped(), spot.spotter.toHtmlEscaped(),
                                     QString::number(spot.freq * 1000.0, 'f', 1), spot.modeGroupString,
                                     spot.comment.toHtmlEscaped(), spot.dateTime.toString("HH:mm"));
        marks.append({ hz, spot.callsign, colour, tip });
        shownSpots.append(spot);
    }
    view->setSpots(marks);
}

void PanadapterWidget::spotClicked(int index)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << index;

    if ( index < 0 || index >= shownSpots.size() )
        return;

    /* NewContact tunes the rig as from the bandmap; move marks now. */
    const DxSpot spot = shownSpots[index];
    moveMarks(std::round(spot.freq * 1e6), pan != 0.0);
    emit tuneDx(spot);
}

void PanadapterWidget::levelsChanged()
{
    FCT_IDENTIFICATION;

    QSettings settings;
    settings.setValue(SETTINGS_RANGE, rangeSlider->value());
    settings.setValue(SETTINGS_LEVEL, levelSlider->value());
    view->setLevels(rangeSlider->value(), levelSlider->value());
}

void PanadapterWidget::showBands()
{
    FCT_IDENTIFICATION;

    QVector<PanadapterView::Segment> allowed;
    QVector<PanadapterView::Segment> segments;

    for ( const auto &band : region1 )
        allowed.append({ band.low * 1000.0, band.high * 1000.0, QColor(255, 255, 255, 70) });

    /* Clip to TX allocations: QLog's band plan exceeds some Region 1 edges. */
    for ( const BandPlan::BandModeRange &range : BandPlan::r1BandModeRanges() )
    {
        QColor colour;
        switch ( range.mode )
        {
        case BandPlan::BAND_MODE_CW:
            colour = QColor(90, 150, 255);
            break;
        case BandPlan::BAND_MODE_DIGITAL:
        case BandPlan::BAND_MODE_FT8:
        case BandPlan::BAND_MODE_FT4:
        case BandPlan::BAND_MODE_FT2:
            colour = QColor(70, 210, 130);
            break;
        case BandPlan::BAND_MODE_LSB:
        case BandPlan::BAND_MODE_USB:
        case BandPlan::BAND_MODE_PHONE:
            colour = QColor(255, 170, 60);
            break;
        default:
            continue;
        }

        for ( const auto &band : region1 )
        {
            const double lowHz = qMax(range.start * 1e6, band.low * 1000.0);
            const double highHz = qMin(range.end * 1e6, band.high * 1000.0);
            if ( highHz > lowHz )
                segments.append({ lowHz, highHz, colour });
        }
    }

    view->setBands(allowed, segments);
}

void PanadapterWidget::updateFrequency(VFOID vfoid, double vfoFreq, double ritFreq, double)
{
    FCT_IDENTIFICATION;

    if ( vfoid != VFO1 )
        return;

    const double newVfo = vfoFreq * 1e6;

    if ( tuneTarget > 0 )
    {
        const qint64 waited = QDateTime::currentMSecsSinceEpoch() - tuneSentAt;

        if ( std::fabs(newVfo - tuneTarget) >= 5.0 && waited < TUNE_SETTLE_MS )
        {
            /* Hold, do not drop: the rig reports only on change, so this may
               be its final frequency if the knob was turned meanwhile. */
            heldVfo = newVfo;
            heldRx = ritFreq * 1e6;
            settleTimer->start(static_cast<int>(TUNE_SETTLE_MS - waited));
            return;
        }

        tuneTarget = 0.0;
        heldVfo = 0.0;
        settleTimer->stop();
    }

    vfo = newVfo;
    rx = ritFreq * 1e6;
    updateWindow();
}

// settle timeout: apply the held report
void PanadapterWidget::settleTuning()
{
    FCT_IDENTIFICATION;

    if ( tuneTarget <= 0 || heldVfo <= 0 )
        return;

    tuneTarget = 0.0;
    vfo = heldVfo;
    rx = heldRx;
    heldVfo = 0.0;
    updateWindow();
}

void PanadapterWidget::updateMode(VFOID vfoid, const QString &raw, const QString &,
                                  const QString &, qint32 width)
{
    FCT_IDENTIFICATION;

    if ( vfoid != VFO1 )
        return;

    rawMode = raw;
    bandwidth = width;
    updateWindow();
}

void PanadapterWidget::updatePTT(VFOID vfoid, bool ptt)
{
    FCT_IDENTIFICATION;

    if ( vfoid != VFO1 )
        return;

    transmitting = ptt;
    view->setTransmitting(ptt);
}

void PanadapterWidget::showStatus()
{
    FCT_IDENTIFICATION;

    if ( config.connection == PanadapterConfig::RigScope )
    {
        statusLabel->setText(scope ? scope->description() : tr("Rig's own scope, through Hamlib"));
        return;
    }

    if ( !device )
    {
        statusLabel->setText(SdrDevice::driverName(config.driver)
                             + ( config.connection == PanadapterConfig::IfOutput
                                 ? tr(" - IF %1 MHz").arg(config.ifCenter / 1e6, 0, 'f', 3)
                                 : tr(" - own antenna") ));
        return;
    }

    statusLabel->setText(QStringLiteral("%1 - %2 - %3 MS/s")
                         .arg(device->description(),
                              config.connection == PanadapterConfig::IfOutput
                                  ? tr("IF %1 MHz").arg(config.ifCenter / 1e6, 0, 'f', 3)
                                  : tr("own antenna"))
                         .arg(device->sampleRate() / 1e6, 0, 'f', 3));
}
