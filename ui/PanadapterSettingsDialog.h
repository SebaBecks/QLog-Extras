#ifndef QLOG_UI_PANADAPTERSETTINGSDIALOG_H
#define QLOG_UI_PANADAPTERSETTINGSDIALOG_H

#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QRadioButton;
class QLabel;
class QGroupBox;

/* Panadapter settings, stored in QSettings (no DB migration). */
struct PanadapterConfig
{
    enum Connection
    {
        /* Dongle fixed on the rig's IF output. */
        IfOutput,
        /* Own antenna or rig RX OUT; dongle retuned to follow the rig. */
        Antenna,
        /* Rig's scope via rigctld multicast, no dongle. */
        RigScope
    };

    QString driver;
    /* Empty = default search. */
    QString library;
    int device = 0;
    int sampleRate = 2400000;
    /* Negative = device AGC. */
    double gain = 29.7;
    int ppm = 0;
    int directSampling = 0;
    bool biasTee = false;

    Connection connection = IfOutput;
    double ifCenter = 9005000.0;
    /* High-side LO mixing inverts the spectrum (FTdx101 MAIN, 9.005 MHz). */
    bool inverted = true;

    /* Hz from the displayed (carrier) frequency to the passband centre. */
    int sidebandOffset = 1500;
    int cwOffset = 0;

    /* 0 = no spots. */
    int spotMinutes = 20;

    /* Must match multicast_data_addr/port in the rig profile. */
    QString scopeGroup = QStringLiteral("224.0.0.1");
    int scopePort = 4535;

    static PanadapterConfig load();
    void save() const;
};

class PanadapterSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PanadapterSettingsDialog(QWidget *parent = nullptr);

    PanadapterConfig config() const;

public slots:
    void accept() override;

private slots:
    void refreshDevices();
    void browseLibrary();
    void presetChosen(int index);
    void connectionChanged();
    void biasTeeToggled(bool on);

private:
    QComboBox *driverCombo;
    QLineEdit *libraryEdit;
    QComboBox *deviceCombo;
    QComboBox *rateCombo;
    QCheckBox *autoGainCheck;
    QDoubleSpinBox *gainSpin;
    QSpinBox *ppmSpin;
    QComboBox *directCombo;
    QRadioButton *ifRadio;
    QRadioButton *antennaRadio;
    QRadioButton *scopeRadio;
    QGroupBox *receiverGroup;
    QGroupBox *ifGroup;
    QComboBox *presetCombo;
    QDoubleSpinBox *ifSpin;
    QCheckBox *invertedCheck;
    QGroupBox *antennaGroup;
    QCheckBox *biasTeeCheck;
    QGroupBox *scopeGroup;
    QLineEdit *scopeGroupEdit;
    QSpinBox *scopePortSpin;
    QSpinBox *sidebandSpin;
    QSpinBox *cwSpin;
    QSpinBox *spotSpin;
    int savedDevice;
};

#endif // QLOG_UI_PANADAPTERSETTINGSDIALOG_H
