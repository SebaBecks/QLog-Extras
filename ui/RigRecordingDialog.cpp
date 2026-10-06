#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include "RigRecordingDialog.h"
#include "digi/AudioInput.h"
#include "rig/QsoRecorder.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.ui.rigrecordingdialog");

#define SETTINGS_DEVICE       "rigpanel/recording/device"
#define SETTINGS_VOICE_SOURCE "rigpanel/recording/voicesource"
#define SETTINGS_VOICE_DEVICE "rigpanel/recording/voicedevice"
#define SETTINGS_STEREO       "rigpanel/recording/stereo"
#define SETTINGS_FOLDER       "rigpanel/recording/folder"
#define SETTINGS_RATE         "rigpanel/recording/rate"
#define SETTINGS_BUFFER       "rigpanel/recording/buffer"

#define SOURCE_RIG   "rig"
#define SOURCE_INPUT "input"

namespace
{

QString profileName;

// inputs are per rig profile; folder, rate and buffer are shared
QString profileKey(const char *key)
{
    FCT_IDENTIFICATION;

    if ( profileName.isEmpty() )
        return QString::fromLatin1(key);

    QString name = profileName;
    name.replace(QChar('/'), QChar('_')).replace(QChar('\\'), QChar('_'));

    return QStringLiteral("rigpanel/profiles/%1/recording/%2")
           .arg(name, QString::fromLatin1(key).section(QChar('/'), -1));
}

// falls back to the shared value
QVariant profileValue(const QSettings &settings, const char *key, const QVariant &fallback)
{
    FCT_IDENTIFICATION;

    return settings.value(profileKey(key), settings.value(QString::fromLatin1(key), fallback));
}

}

void RigRecordingDialog::setProfile(const QString &name)
{
    FCT_IDENTIFICATION;

    profileName = name;
}

RigRecordingDialog::RigRecordingDialog(QWidget *parent) :
    QDialog(parent),
    deviceCombo(new QComboBox(this)),
    voiceSourceCombo(new QComboBox(this)),
    voiceDeviceCombo(new QComboBox(this)),
    channelsCombo(new QComboBox(this)),
    folderEdit(new QLineEdit(this)),
    rateCombo(new QComboBox(this)),
    bufferSpin(new QSpinBox(this))
{
    FCT_IDENTIFICATION;

    setWindowTitle(profileName.isEmpty() ? tr("Recording")
                                         : tr("Recording - %1").arg(profileName));
    setMinimumWidth(560);

    QSettings settings;
    const QStringList inputs = AudioInput::inputDeviceNames();

    deviceCombo->addItems(inputs);
    deviceCombo->setCurrentIndex(qMax(0, deviceCombo->findText(device())));

    voiceSourceCombo->addItem(tr("Through the rig - its transmit monitor (MONI) on"),
                              QStringLiteral(SOURCE_RIG));
    voiceSourceCombo->addItem(tr("From a separate input"), QStringLiteral(SOURCE_INPUT));
    voiceSourceCombo->setCurrentIndex(qMax(0, voiceSourceCombo->findData(
        profileValue(settings, SETTINGS_VOICE_SOURCE, QStringLiteral(SOURCE_RIG)).toString())));
    voiceSourceCombo->setToolTip(tr("Where your own voice comes from. The rig gives it "
                                    "only while its transmit monitor is on - and then "
                                    "you hear yourself in the headphones too. A separate "
                                    "input - a microphone equaliser's monitor output, or "
                                    "the computer's microphone if you transmit from it - "
                                    "is let into the recording only while the rig "
                                    "transmits, so the room stays out of it."));

    voiceDeviceCombo->addItems(inputs);
    voiceDeviceCombo->setCurrentIndex(qMax(0, voiceDeviceCombo->findText(
        profileValue(settings, SETTINGS_VOICE_DEVICE, QString()).toString())));

    channelsCombo->addItem(tr("Stereo - the other station left, you right"), true);
    channelsCombo->addItem(tr("Mono - both mixed"), false);
    channelsCombo->setCurrentIndex(qMax(0, channelsCombo->findData(stereo())));
    channelsCombo->setToolTip(tr("Two channels keep the two sides apart, so either can "
                                 "be made louder later. Applies to a separate voice "
                                 "input; through the rig there is only one channel."));

    connect(voiceSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RigRecordingDialog::voiceSourceChosen);
    voiceSourceChosen();

    folderEdit->setText(folder());

    QPushButton *browseButton = new QPushButton(tr("Browse..."), this);
    connect(browseButton, &QPushButton::clicked, this, &RigRecordingDialog::browse);

    QHBoxLayout *folderRow = new QHBoxLayout;
    folderRow->addWidget(folderEdit);
    folderRow->addWidget(browseButton);

    rateCombo->addItem(tr("16 kHz - speech, a third of the size"), 16000);
    rateCombo->addItem(tr("48 kHz - wide AM and FM"), 48000);
    rateCombo->setCurrentIndex(qMax(0, rateCombo->findData(rate())));

    bufferSpin->setRange(0, 120);
    bufferSpin->setSuffix(tr(" s"));
    bufferSpin->setValue(bufferSeconds());
    bufferSpin->setToolTip(tr("How much of what came before the press goes into "
                              "the recording, so the start of a contact is not "
                              "lost to reaching for the button."));

    QFormLayout *form = new QFormLayout;
    form->addRow(tr("Record from"), deviceCombo);
    form->addRow(tr("Your voice"), voiceSourceCombo);
    form->addRow(tr("Voice input"), voiceDeviceCombo);
    form->addRow(tr("Channels"), channelsCombo);
    form->addRow(tr("Save to"), folderRow);
    form->addRow(tr("Quality"), rateCombo);
    form->addRow(tr("Start from"), bufferSpin);

    QLabel *help = new QLabel(tr("Record from whatever carries the rig's receive "
                                 "audio into this computer - the same card the "
                                 "Digi Panel listens to. To have your own voice in "
                                 "the recording through the rig, switch its transmit "
                                 "monitor (MONI) on; with it off the rig sends "
                                 "nothing while transmitting. Files are named after "
                                 "the contact: date and time in UTC, callsign, band "
                                 "and mode."), this);
    help->setWordWrap(true);

    QDialogButtonBox *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    connect(buttons, &QDialogButtonBox::accepted, this, &RigRecordingDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &RigRecordingDialog::reject);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(help);
    layout->addStretch();
    layout->addWidget(buttons);
}

// voice input and channels apply only to a separate voice source
void RigRecordingDialog::voiceSourceChosen()
{
    FCT_IDENTIFICATION;

    const bool separate = ( voiceSourceCombo->currentData().toString() == SOURCE_INPUT );

    voiceDeviceCombo->setEnabled(separate);
    channelsCombo->setEnabled(separate);
}

QString RigRecordingDialog::device()
{
    FCT_IDENTIFICATION;

    return profileValue(QSettings(), SETTINGS_DEVICE, QString()).toString();
}

QString RigRecordingDialog::voiceDevice()
{
    FCT_IDENTIFICATION;

    const QSettings settings;

    if ( profileValue(settings, SETTINGS_VOICE_SOURCE, QStringLiteral(SOURCE_RIG)).toString() != SOURCE_INPUT )
        return QString();

    return profileValue(settings, SETTINGS_VOICE_DEVICE, QString()).toString();
}

bool RigRecordingDialog::stereo()
{
    FCT_IDENTIFICATION;

    return profileValue(QSettings(), SETTINGS_STEREO, true).toBool();
}

QString RigRecordingDialog::folder()
{
    FCT_IDENTIFICATION;

    const QString chosen = QSettings().value(SETTINGS_FOLDER).toString();

    return chosen.isEmpty() ? QsoRecorder::defaultFolder() : chosen;
}

int RigRecordingDialog::rate()
{
    FCT_IDENTIFICATION;

    return QSettings().value(SETTINGS_RATE, 16000).toInt();
}

int RigRecordingDialog::bufferSeconds()
{
    FCT_IDENTIFICATION;

    return QSettings().value(SETTINGS_BUFFER, 30).toInt();
}

void RigRecordingDialog::browse()
{
    FCT_IDENTIFICATION;

    const QString chosen = QFileDialog::getExistingDirectory(this, tr("Save recordings to"),
                                                             folderEdit->text());

    if ( !chosen.isEmpty() )
        folderEdit->setText(chosen);
}

void RigRecordingDialog::accept()
{
    FCT_IDENTIFICATION;

    QSettings settings;

    settings.setValue(profileKey(SETTINGS_DEVICE), deviceCombo->currentText());
    settings.setValue(profileKey(SETTINGS_VOICE_SOURCE), voiceSourceCombo->currentData().toString());
    settings.setValue(profileKey(SETTINGS_VOICE_DEVICE), voiceDeviceCombo->currentText());
    settings.setValue(profileKey(SETTINGS_STEREO), channelsCombo->currentData().toBool());
    settings.setValue(SETTINGS_FOLDER, folderEdit->text().trimmed());
    settings.setValue(SETTINGS_RATE, rateCombo->currentData().toInt());
    settings.setValue(SETTINGS_BUFFER, bufferSpin->value());

    QDialog::accept();
}
