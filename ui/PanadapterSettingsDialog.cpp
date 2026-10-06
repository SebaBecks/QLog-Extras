#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include "PanadapterSettingsDialog.h"
#include "sdr/SdrDevice.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.ui.panadaptersettingsdialog");

#define SETTINGS_DRIVER      "panadapter/driver"
#define SETTINGS_LIBRARY     "panadapter/library"
#define SETTINGS_DEVICE      "panadapter/device"
#define SETTINGS_RATE        "panadapter/rate"
#define SETTINGS_GAIN        "panadapter/gain"
#define SETTINGS_PPM         "panadapter/ppm"
#define SETTINGS_DIRECT      "panadapter/directsampling"
#define SETTINGS_BIASTEE     "panadapter/biastee"
#define SETTINGS_CONNECTION  "panadapter/connection"
#define SETTINGS_IFCENTER    "panadapter/ifcenter"
#define SETTINGS_INVERTED    "panadapter/inverted"
#define SETTINGS_SIDEBAND    "panadapter/sidebandoffset"
#define SETTINGS_CW          "panadapter/cwoffset"
#define SETTINGS_SPOTS       "panadapter/spotminutes"
#define SETTINGS_SCOPEGROUP  "panadapter/scopegroup"
#define SETTINGS_SCOPEPORT   "panadapter/scopeport"

#define CONNECTION_IF        "if"
#define CONNECTION_ANTENNA   "antenna"
#define CONNECTION_SCOPE     "scope"

/* Measured IF outputs only; a wrong preset would misplace every station. */
static const struct { const char *name; double hz; bool inverted; } presets[] = {
    { QT_TRANSLATE_NOOP("PanadapterSettingsDialog", "Yaesu FTdx101 - MAIN (9.005 MHz)"), 9005000.0, true },
    { QT_TRANSLATE_NOOP("PanadapterSettingsDialog", "Yaesu FTdx101 - SUB (8.900 MHz)"), 8900000.0, true },
};

PanadapterConfig PanadapterConfig::load()
{
    FCT_IDENTIFICATION;

    QSettings settings;
    PanadapterConfig c;

    c.driver = settings.value(SETTINGS_DRIVER, QStringLiteral("rtlsdr")).toString();
    c.library = settings.value(SETTINGS_LIBRARY).toString();
    c.device = settings.value(SETTINGS_DEVICE, 0).toInt();
    c.sampleRate = settings.value(SETTINGS_RATE, 2400000).toInt();
    c.gain = settings.value(SETTINGS_GAIN, 29.7).toDouble();
    c.ppm = settings.value(SETTINGS_PPM, 0).toInt();
    c.directSampling = settings.value(SETTINGS_DIRECT, 0).toInt();
    c.biasTee = settings.value(SETTINGS_BIASTEE, false).toBool();
    const QString connection = settings.value(SETTINGS_CONNECTION, CONNECTION_IF).toString();
    c.connection = connection == QLatin1String(CONNECTION_ANTENNA) ? Antenna
                 : connection == QLatin1String(CONNECTION_SCOPE)   ? RigScope
                                                                   : IfOutput;
    c.ifCenter = settings.value(SETTINGS_IFCENTER, 9005000.0).toDouble();
    c.inverted = settings.value(SETTINGS_INVERTED, true).toBool();
    c.sidebandOffset = settings.value(SETTINGS_SIDEBAND, 1500).toInt();
    c.cwOffset = settings.value(SETTINGS_CW, 0).toInt();
    c.spotMinutes = settings.value(SETTINGS_SPOTS, 20).toInt();
    c.scopeGroup = settings.value(SETTINGS_SCOPEGROUP, c.scopeGroup).toString();
    c.scopePort = settings.value(SETTINGS_SCOPEPORT, c.scopePort).toInt();

    /* Bias tee only on an antenna; never into a rig. */
    if ( c.connection != Antenna )
        c.biasTee = false;
    return c;
}

void PanadapterConfig::save() const
{
    FCT_IDENTIFICATION;

    QSettings settings;

    settings.setValue(SETTINGS_DRIVER, driver);
    settings.setValue(SETTINGS_LIBRARY, library);
    settings.setValue(SETTINGS_DEVICE, device);
    settings.setValue(SETTINGS_RATE, sampleRate);
    settings.setValue(SETTINGS_GAIN, gain);
    settings.setValue(SETTINGS_PPM, ppm);
    settings.setValue(SETTINGS_DIRECT, directSampling);
    settings.setValue(SETTINGS_BIASTEE, connection == Antenna && biasTee);
    settings.setValue(SETTINGS_CONNECTION, connection == Antenna  ? CONNECTION_ANTENNA
                                         : connection == RigScope ? CONNECTION_SCOPE
                                                                  : CONNECTION_IF);
    settings.setValue(SETTINGS_SCOPEGROUP, scopeGroup);
    settings.setValue(SETTINGS_SCOPEPORT, scopePort);
    settings.setValue(SETTINGS_IFCENTER, ifCenter);
    settings.setValue(SETTINGS_INVERTED, inverted);
    settings.setValue(SETTINGS_SIDEBAND, sidebandOffset);
    settings.setValue(SETTINGS_CW, cwOffset);
    settings.setValue(SETTINGS_SPOTS, spotMinutes);
}

PanadapterSettingsDialog::PanadapterSettingsDialog(QWidget *parent) :
    QDialog(parent),
    driverCombo(new QComboBox(this)),
    libraryEdit(new QLineEdit(this)),
    deviceCombo(new QComboBox(this)),
    rateCombo(new QComboBox(this)),
    autoGainCheck(new QCheckBox(tr("Automatic"), this)),
    gainSpin(new QDoubleSpinBox(this)),
    ppmSpin(new QSpinBox(this)),
    directCombo(new QComboBox(this)),
    ifRadio(new QRadioButton(tr("Rig's IF output"), this)),
    antennaRadio(new QRadioButton(tr("Own antenna, or the rig's RX OUT"), this)),
    scopeRadio(new QRadioButton(tr("No receiver - the rig's own scope (Icom, through Hamlib)"), this)),
    receiverGroup(new QGroupBox(tr("Receiver"), this)),
    ifGroup(new QGroupBox(tr("IF output"), this)),
    presetCombo(new QComboBox(this)),
    ifSpin(new QDoubleSpinBox(this)),
    invertedCheck(new QCheckBox(tr("The band comes out inverted"), this)),
    antennaGroup(new QGroupBox(tr("Antenna"), this)),
    biasTeeCheck(new QCheckBox(tr("Bias tee - power for an active antenna or LNA"), this)),
    scopeGroup(new QGroupBox(tr("Rig's scope"), this)),
    scopeGroupEdit(new QLineEdit(this)),
    scopePortSpin(new QSpinBox(this)),
    sidebandSpin(new QSpinBox(this)),
    cwSpin(new QSpinBox(this)),
    spotSpin(new QSpinBox(this))
{
    FCT_IDENTIFICATION;

    setWindowTitle(tr("Panadapter"));
    setMinimumWidth(560);

    const PanadapterConfig c = PanadapterConfig::load();
    savedDevice = c.device;

    /* Receiver */
    for ( const QString &key : SdrDevice::driverKeys() )
        driverCombo->addItem(SdrDevice::driverName(key), key);
    driverCombo->setCurrentIndex(qMax(0, driverCombo->findData(c.driver)));

    libraryEdit->setText(c.library);
    libraryEdit->setPlaceholderText(tr("Automatic - rtlsdr.dll next to QLog"));
    libraryEdit->setToolTip(tr("The receiver's library. For an RTL-SDR Blog V4 it has to be "
                               "the one from RTL-SDR Blog, which knows its tuner and its "
                               "HF upconverter."));
    QPushButton *browseButton = new QPushButton(tr("Browse..."), this);
    QHBoxLayout *libraryRow = new QHBoxLayout;
    libraryRow->addWidget(libraryEdit, 1);
    libraryRow->addWidget(browseButton);

    QPushButton *refreshButton = new QPushButton(tr("Refresh"), this);
    QHBoxLayout *deviceRow = new QHBoxLayout;
    deviceRow->addWidget(deviceCombo, 1);
    deviceRow->addWidget(refreshButton);

    for ( int rate : { 2400000, 2048000, 1024000 } )
        rateCombo->addItem(tr("%1 MS/s").arg(rate / 1e6, 0, 'f', 3), rate);
    rateCombo->setCurrentIndex(qMax(0, rateCombo->findData(c.sampleRate)));
    rateCombo->setToolTip(tr("How wide a slice of the band the receiver takes in at once."));

    gainSpin->setRange(0.0, 50.0);
    gainSpin->setDecimals(1);
    gainSpin->setSuffix(tr(" dB"));
    gainSpin->setValue(c.gain < 0 ? 29.7 : c.gain);
    autoGainCheck->setChecked(c.gain < 0);
    gainSpin->setEnabled(c.gain >= 0);
    QHBoxLayout *gainRow = new QHBoxLayout;
    gainRow->addWidget(gainSpin, 1);
    gainRow->addWidget(autoGainCheck);

    ppmSpin->setRange(-200, 200);
    ppmSpin->setSuffix(tr(" ppm"));
    ppmSpin->setValue(c.ppm);
    ppmSpin->setToolTip(tr("Correction for the receiver's crystal. Only matters on an own "
                           "antenna; behind a rig's IF the rig sets the frequency."));

    directCombo->addItem(tr("Off"), 0);
    directCombo->addItem(tr("I branch"), 1);
    directCombo->addItem(tr("Q branch"), 2);
    directCombo->setCurrentIndex(qMax(0, directCombo->findData(c.directSampling)));
    directCombo->setToolTip(tr("How an RTL-SDR V3 or older reaches HF without an upconverter "
                               "(usually Q). Leave it off on a V4."));

    QFormLayout *receiverForm = new QFormLayout(receiverGroup);
    receiverForm->addRow(tr("Driver"), driverCombo);
    receiverForm->addRow(tr("Library"), libraryRow);
    receiverForm->addRow(tr("Device"), deviceRow);
    receiverForm->addRow(tr("Sample rate"), rateCombo);
    receiverForm->addRow(tr("Gain"), gainRow);
    receiverForm->addRow(tr("Frequency correction"), ppmSpin);
    receiverForm->addRow(tr("Direct sampling"), directCombo);

    /* Connection */
    ifRadio->setChecked(c.connection == PanadapterConfig::IfOutput);
    antennaRadio->setChecked(c.connection == PanadapterConfig::Antenna);
    scopeRadio->setChecked(c.connection == PanadapterConfig::RigScope);
    scopeRadio->setToolTip(tr("For a rig with no IF or RX output to put a receiver behind, "
                              "such as the IC-705: the picture is the rig's own scope."));
    QGroupBox *connectionGroup = new QGroupBox(tr("Connected to"), this);
    QVBoxLayout *connectionLayout = new QVBoxLayout(connectionGroup);
    connectionLayout->addWidget(ifRadio);
    connectionLayout->addWidget(antennaRadio);
    connectionLayout->addWidget(scopeRadio);

    for ( const auto &preset : presets )
        presetCombo->addItem(tr(preset.name), preset.hz);
    presetCombo->addItem(tr("Other rig"), 0.0);
    int presetIndex = presetCombo->count() - 1;
    for ( int i = 0; i < int(sizeof(presets) / sizeof(presets[0])); i++ )
        if ( qFuzzyCompare(presets[i].hz, c.ifCenter) )
            presetIndex = i;
    presetCombo->setCurrentIndex(presetIndex);

    ifSpin->setRange(100.0, 2000000.0);
    ifSpin->setDecimals(3);
    ifSpin->setSuffix(tr(" kHz"));
    ifSpin->setValue(c.ifCenter / 1000.0);
    invertedCheck->setChecked(c.inverted);
    invertedCheck->setToolTip(tr("A station tuned in LSB has to be heard as USB on the IF. "
                                 "True for the FTdx101."));

    QFormLayout *ifForm = new QFormLayout(ifGroup);
    ifForm->addRow(tr("Rig"), presetCombo);
    ifForm->addRow(tr("IF frequency"), ifSpin);
    ifForm->addRow(QString(), invertedCheck);

    biasTeeCheck->setChecked(c.biasTee);
    QLabel *antennaWarning = new QLabel(tr("<b>Protect the receiver from your own transmitter.</b> "
                                           "An antenna shared through a splitter, or one close to "
                                           "the transmitting antenna, will destroy the receiver "
                                           "at 100 W. Use a transmit/receive switch driven from "
                                           "the rig's TX GND, or an RX OUT that the rig mutes "
                                           "while transmitting. QLog only stops drawing; it "
                                           "cannot protect the hardware."), this);
    antennaWarning->setWordWrap(true);
    antennaWarning->setStyleSheet(QStringLiteral("color: #b00020;"));
    QVBoxLayout *antennaLayout = new QVBoxLayout(antennaGroup);
    antennaLayout->addWidget(antennaWarning);
    antennaLayout->addWidget(biasTeeCheck);

    scopeGroupEdit->setText(c.scopeGroup);
    scopePortSpin->setRange(1024, 65535);
    scopePortSpin->setValue(c.scopePort);

    /* Selectable text with the rigctld arguments to copy into the rig profile. */
    QLabel *scopeHelp = new QLabel(this);
    scopeHelp->setWordWrap(true);
    scopeHelp->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFormLayout *scopeForm = new QFormLayout(scopeGroup);
    scopeForm->addRow(tr("Multicast group"), scopeGroupEdit);
    scopeForm->addRow(tr("Port"), scopePortSpin);
    scopeForm->addRow(scopeHelp);

    auto updateScopeHelp = [this, scopeHelp]()
    {
        scopeHelp->setText(tr("The rig's Hamlib profile needs <b>Share Rig via port</b>, and under "
                              "<b>Advanced...</b> these additional arguments:<br>"
                              "<code>-C async=1,multicast_data_addr=%1,multicast_data_port=%2</code><br>"
                              "An IC-705 also needs <i>CI-V USB Port</i> set to <i>Unlink from "
                              "[REMOTE]</i>. The panadapter switches the rig's scope on while it "
                              "runs and back as it was afterwards.")
                           .arg(scopeGroupEdit->text().trimmed()).arg(scopePortSpin->value()));
    };
    updateScopeHelp();
    connect(scopeGroupEdit, &QLineEdit::textChanged, this, updateScopeHelp);
    connect(scopePortSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, updateScopeHelp);

    /* Tuning */
    sidebandSpin->setRange(0, 3000);
    sidebandSpin->setSuffix(tr(" Hz"));
    sidebandSpin->setValue(c.sidebandOffset);
    sidebandSpin->setToolTip(tr("How far the middle of an SSB passband lies from the displayed "
                                "frequency - up in USB, down in LSB. A click on a station "
                                "puts its middle there."));
    cwSpin->setRange(-2000, 2000);
    cwSpin->setSuffix(tr(" Hz"));
    cwSpin->setValue(c.cwOffset);
    cwSpin->setToolTip(tr("The same for CW. Zero when the rig displays the frequency of the "
                          "signal it is tuned to."));
    spotSpin->setRange(0, 120);
    spotSpin->setSuffix(tr(" min"));
    spotSpin->setSpecialValueText(tr("Do not show"));
    spotSpin->setValue(c.spotMinutes);
    spotSpin->setToolTip(tr("How long a DX cluster spot stays in the picture. The spots are "
                            "the ones that pass the DX cluster's filters, as in the bandmap."));

    QGroupBox *tuningGroup = new QGroupBox(tr("Tuning and spots"), this);
    QFormLayout *tuningForm = new QFormLayout(tuningGroup);
    tuningForm->addRow(tr("SSB passband middle"), sidebandSpin);
    tuningForm->addRow(tr("CW offset"), cwSpin);
    tuningForm->addRow(tr("DX spots shown for"), spotSpin);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(receiverGroup);
    layout->addWidget(connectionGroup);
    layout->addWidget(ifGroup);
    layout->addWidget(antennaGroup);
    layout->addWidget(scopeGroup);
    layout->addWidget(tuningGroup);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &PanadapterSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browseButton, &QPushButton::clicked, this, &PanadapterSettingsDialog::browseLibrary);
    connect(refreshButton, &QPushButton::clicked, this, &PanadapterSettingsDialog::refreshDevices);
    connect(autoGainCheck, &QCheckBox::toggled, gainSpin, [this](bool on) { gainSpin->setEnabled(!on); });
    connect(presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PanadapterSettingsDialog::presetChosen);
    // any of the three may become checked
    for ( QRadioButton *radio : { ifRadio, antennaRadio, scopeRadio } )
        connect(radio, &QRadioButton::toggled, this, &PanadapterSettingsDialog::connectionChanged);
    connect(biasTeeCheck, &QCheckBox::toggled, this, &PanadapterSettingsDialog::biasTeeToggled);

    connectionChanged();
    refreshDevices();
}

void PanadapterSettingsDialog::refreshDevices()
{
    FCT_IDENTIFICATION;

    const int keep = deviceCombo->count() ? deviceCombo->currentIndex() : savedDevice;
    SdrDevice *probe = SdrDevice::create(driverCombo->currentData().toString(), this);

    deviceCombo->clear();
    if ( !probe )
        return;

    probe->setLibraryPath(libraryEdit->text().trimmed());
    const QStringList names = probe->deviceNames();

    if ( names.isEmpty() )
    {
        deviceCombo->addItem(probe->lastError().isEmpty() ? tr("No receiver found") : probe->lastError(), 0);
        deviceCombo->setToolTip(probe->lastError());
    }
    else
    {
        for ( int i = 0; i < names.size(); i++ )
            deviceCombo->addItem(names[i], i);
        deviceCombo->setCurrentIndex(qBound(0, keep, names.size() - 1));
        deviceCombo->setToolTip(tr("Library: %1").arg(probe->libraryPath()));
    }
    delete probe;
}

void PanadapterSettingsDialog::browseLibrary()
{
    FCT_IDENTIFICATION;

    const QString path = QFileDialog::getOpenFileName(this, tr("Receiver library"), libraryEdit->text(),
#ifdef Q_OS_WIN
                                                      tr("Libraries (*.dll)")
#else
                                                      tr("Libraries (*.so *.so.* *.dylib)")
#endif
                                                      );
    if ( path.isEmpty() )
        return;
    libraryEdit->setText(path);
    refreshDevices();
}

void PanadapterSettingsDialog::presetChosen(int index)
{
    FCT_IDENTIFICATION;

    const double hz = presetCombo->itemData(index).toDouble();
    if ( hz <= 0 )
        return;
    ifSpin->setValue(hz / 1000.0);
    if ( index >= 0 && index < int(sizeof(presets) / sizeof(presets[0])) )
        invertedCheck->setChecked(presets[index].inverted);
}

void PanadapterSettingsDialog::connectionChanged()
{
    FCT_IDENTIFICATION;

    const bool behindRig = ifRadio->isChecked();
    const bool rigScope = scopeRadio->isChecked();

    receiverGroup->setVisible(!rigScope);
    ifGroup->setVisible(behindRig);
    antennaGroup->setVisible(antennaRadio->isChecked());
    scopeGroup->setVisible(rigScope);
    if ( !antennaRadio->isChecked() )
        biasTeeCheck->setChecked(false);
    adjustSize();
}

void PanadapterSettingsDialog::biasTeeToggled(bool on)
{
    FCT_IDENTIFICATION;

    if ( !on )
        return;

    const auto answer = QMessageBox::warning(this, tr("Bias tee"),
                                             tr("The bias tee puts about 4.5 V on the antenna "
                                                "cable. Switch it on only for an active antenna "
                                                "or an LNA that wants it - never with the cable "
                                                "going into a rig.\n\nSwitch it on?"),
                                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if ( answer != QMessageBox::Yes )
        biasTeeCheck->setChecked(false);
}

PanadapterConfig PanadapterSettingsDialog::config() const
{
    FCT_IDENTIFICATION;

    PanadapterConfig c;

    c.driver = driverCombo->currentData().toString();
    c.library = libraryEdit->text().trimmed();
    c.device = deviceCombo->currentData().toInt();
    c.sampleRate = rateCombo->currentData().toInt();
    c.gain = autoGainCheck->isChecked() ? -1.0 : gainSpin->value();
    c.ppm = ppmSpin->value();
    c.directSampling = directCombo->currentData().toInt();
    c.connection = antennaRadio->isChecked() ? PanadapterConfig::Antenna
                 : scopeRadio->isChecked()   ? PanadapterConfig::RigScope
                                             : PanadapterConfig::IfOutput;
    c.scopeGroup = scopeGroupEdit->text().trimmed();
    c.scopePort = scopePortSpin->value();
    c.biasTee = c.connection == PanadapterConfig::Antenna && biasTeeCheck->isChecked();
    c.ifCenter = ifSpin->value() * 1000.0;
    c.inverted = invertedCheck->isChecked();
    c.sidebandOffset = sidebandSpin->value();
    c.cwOffset = cwSpin->value();
    c.spotMinutes = spotSpin->value();
    return c;
}

void PanadapterSettingsDialog::accept()
{
    FCT_IDENTIFICATION;

    config().save();
    QDialog::accept();
}
