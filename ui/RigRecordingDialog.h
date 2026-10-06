#ifndef QLOG_UI_RIGRECORDINGDIALOG_H
#define QLOG_UI_RIGRECORDINGDIALOG_H

#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;

/* Recording sources and output settings. Separate from the main settings
   dialog, which upstream changes often. */
class RigRecordingDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RigRecordingDialog(QWidget *parent = nullptr);

    // input settings are stored per rig profile
    static void setProfile(const QString &name);

    static QString device();
    // empty when the voice comes through the rig's monitor
    static QString voiceDevice();
    static bool stereo();
    static QString folder();
    static int rate();
    static int bufferSeconds();

private slots:
    void accept() override;
    void browse();
    void voiceSourceChosen();

private:
    QComboBox *deviceCombo;
    QComboBox *voiceSourceCombo;
    QComboBox *voiceDeviceCombo;
    QComboBox *channelsCombo;
    QLineEdit *folderEdit;
    QComboBox *rateCombo;
    QSpinBox *bufferSpin;
};

#endif // QLOG_UI_RIGRECORDINGDIALOG_H
