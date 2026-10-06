#ifndef QLOG_UI_PANADAPTERWIDGET_H
#define QLOG_UI_PANADAPTERWIDGET_H

#include <QWidget>
#include <QThread>

#include "PanadapterSettingsDialog.h"
#include "rig/drivers/GenericRigDrv.h"
#include "data/DxSpot.h"

class QToolButton;
class QComboBox;
class QLabel;
class QSlider;
class QTimer;
class PanadapterView;
class SdrDevice;
class IqSpectrum;
class RigScopeSource;

/* Panadapter dock: spectrum and waterfall around the rig frequency; click tunes.
   Sources: SDR on the rig's IF output or an antenna, or the rig's own scope
   via rigctld. Rig frequency comes from QLog's rig connection. */
class PanadapterWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PanadapterWidget(QWidget *parent = nullptr);
    ~PanadapterWidget() override;

public slots:
    void updateFrequency(VFOID vfoid, double vfoFreq, double ritFreq, double xitFreq);
    void settleTuning();
    void updateMode(VFOID vfoid, const QString &rawMode, const QString &mode,
                    const QString &subMode, qint32 width);
    void updatePTT(VFOID vfoid, bool ptt);
    /* Already filtered by the DX cluster. */
    void addSpot(const DxSpot &spot);

signals:
    /* Same handling as a bandmap click. */
    void tuneDx(DxSpot spot);

private slots:
    void runToggled(bool on);
    void openSettings();
    void spanChosen(int index);
    void levelsChanged();
    void spectrumArrived(const QVector<float> &decibels);
    void scopeLineArrived(const QVector<float> &decibels, double firstHz, double binHz);
    void viewClicked(double hz);
    void wheelTurned(int steps, Qt::KeyboardModifiers modifiers);
    void recentre();
    void viewDragged(double hz);
    void receiverFailed(const QString &reason);
    void spotClicked(int index);
    void showSpots();

private:
    bool startReceiver();
    bool startRigScope();
    void stopReceiver();
    bool running() const;
    /* Usable receiver bandwidth or rig scope span, halved. */
    double widestHalf() const;
    /* Hz from the rig frequency to the passband centre. */
    double passbandOffset() const;
    double passbandWidth() const;
    /* Voice: click = carrier, as on rig touch scopes; CW/RTTY/data: click =
       passband centre. */
    bool clickedOnCarrier() const;
    double wheelStep() const;
    /* Moves the marks at once, without waiting for the rig's report.
       keepPicture: the band stays put and the marks move over it. */
    void tuneTo(double newVfo, bool keepPicture);
    /* tuneTo() without commanding the rig. */
    void moveMarks(double newVfo, bool keepPicture);
    double halfSpan() const;
    /* IF output: limited to receiver bandwidth; antenna: unlimited. */
    double clampPan(double pan) const;
    /* "Band" span: whole amateur band, fixed while tuning across it. */
    bool showingBand() const;
    /* IARU Region 1 band edges in Hz; false outside the bands. */
    bool bandAround(double hz, double &lowHz, double &highHz) const;
    void updateWindow();
    void configureSpectrum();
    void showStatus();
    void showBands();

    QToolButton *runButton;
    QComboBox *spanCombo;
    QSlider *rangeSlider;
    QSlider *levelSlider;
    QLabel *statusLabel;
    QToolButton *settingsButton;
    PanadapterView *view;

    PanadapterConfig config;
    SdrDevice *device = nullptr;
    IqSpectrum *spectrum = nullptr;
    RigScopeSource *scope = nullptr;
    /* Last scope sweep edges; 0 before the first one. */
    double scopeLow = 0.0;
    double scopeHigh = 0.0;
    QThread worker;

    double vfo = 0.0;
    double rx = 0.0;
    QString rawMode;
    qint32 bandwidth = 0;
    bool transmitting = false;
    double receiverCenter = 0.0;
    /* Window offset from the rig frequency, Hz. */
    double pan = 0.0;
    /* Same for the "Band" span, from the band centre; reset on band change. */
    double bandPan = 0.0;
    double lastBandLow = 0.0;
    /* Pending tune until the rig confirms; stale reports must not pull the
       marks back. */
    double tuneTarget = 0.0;
    QList<DxSpot> spots;
    /* Same order as given to the view, indexed by spotClicked(). */
    QList<DxSpot> shownSpots;
    qint64 tuneSentAt = 0;
    /* Last report differing from tuneTarget while settling; applied after. */
    double heldVfo = 0.0;
    double heldRx = 0.0;
    QTimer *settleTimer = nullptr;
};

#endif // QLOG_UI_PANADAPTERWIDGET_H
