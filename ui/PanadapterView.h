#ifndef QLOG_UI_PANADAPTERVIEW_H
#define QLOG_UI_PANADAPTERVIEW_H

#include <QWidget>
#include <QImage>
#include <QVector>
#include <QColor>

/* Spectrum, frequency scale and waterfall in on-air frequencies; no rig logic.
   Drawn in device pixels so HiDPI scaling does not blur the waterfall.
   Waterfall shifts on retune, restarts on zoom. */
class PanadapterView : public QWidget
{
    Q_OBJECT

public:
    struct Segment
    {
        double low;
        double high;
        QColor colour;
    };

    struct SpotMark
    {
        double hz;
        QString label;
        QColor colour;
        QString tip;
    };

    explicit PanadapterView(QWidget *parent = nullptr);

    void setWindow(double lowHz, double highHz);
    double windowLow() const { return low; }
    double windowHigh() const { return high; }
    /* Width in device pixels. */
    int pixels() const;

    /* rxHz 0 = hidden. */
    void setTuning(double rxHz, double passLowHz, double passHighHz);
    /* Freezes the waterfall; the receiver is muted during TX. */
    void setTransmitting(bool on);
    void setMessage(const QString &text);
    /* Colour span in dB and its start relative to the noise floor. */
    void setLevels(double rangeDb, double offsetDb);
    /* allowed: TX privileges; segments: band plan. */
    void setBands(const QVector<Segment> &allowed, const QVector<Segment> &segments);
    /* Passband preview under the pointer, offsets in Hz from it. */
    void setPointerPassband(double lowOffsetHz, double highOffsetHz);
    void setSpots(const QVector<SpotMark> &marks);

    QSize sizeHint() const override { return QSize(600, 300); }
    QSize minimumSizeHint() const override { return QSize(200, 120); }

public slots:
    void addSpectrum(const QVector<float> &decibels, double firstHz, double binHz);
    void clear();

signals:
    void clicked(double hz);
    /* Index into setSpots() marks. */
    void spotClicked(int index);
    /* Hz to shift the window up (negative: down). */
    void dragged(double hz);
    void doubleClicked();
    /* Positive = up. */
    void wheelTurned(int steps, Qt::KeyboardModifiers modifiers);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRect spectrumArea() const;
    QRect scaleArea() const;
    QRect waterfallArea() const;
    QSize waterfallPixels() const;
    double hzAt(double x) const;
    double xAt(double hz) const;
    double bottomLevel() const;
    double topLevel() const;
    QRgb colourFor(float db) const;
    void buildPalette();

    double low = 0.0;
    double high = 0.0;
    double rx = 0.0;
    double passLow = 0.0;
    double passHigh = 0.0;
    bool transmitting = false;
    QString message;
    double range = 30.0;
    double offset = 0.0;
    QVector<Segment> allowedBands;
    QVector<Segment> bandSegments;
    double pointerLow = 0.0;
    double pointerHigh = 0.0;
    QVector<SpotMark> spotMarks;
    /* Last drawn label rects; empty if it did not fit. */
    QVector<QRectF> spotRects;
    int spotAt(const QPointF &point) const;

    /* One value per device pixel column. */
    QVector<float> line;
    /* Tracked noise floor; keeps colours stable across band/gain changes. */
    double floor = 0.0;
    bool floorKnown = false;

    QImage waterfall;
    QVector<QRgb> palette;
    int hoverX = -1;
    /* Press = click if released in place, drag after a few pixels. */
    bool pressed = false;
    bool dragging = false;
    QPointF pressPoint;
    double lastDragX = 0.0;
    int pressSpot = -1;
    int wheelPending = 0;
};

#endif // QLOG_UI_PANADAPTERVIEW_H
