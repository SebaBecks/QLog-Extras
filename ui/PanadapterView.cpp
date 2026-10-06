#include <cmath>
#include <cstring>
#include <algorithm>

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QToolTip>

#include "PanadapterView.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.ui.panadapterview");

#define SCALE_HEIGHT       20
#define BAND_STRIP         3
#define SPECTRUM_SHARE     0.38
/* ~2 s time constant at 10 spectra/s; a keyed carrier does not shift colours. */
#define FLOOR_FOLLOW       0.05
#define NO_DATA            -1000.0f
/* Pixels before a press becomes a drag. */
#define DRAG_THRESHOLD     4.0

static QPointF mousePoint(const QMouseEvent *event)
{
    FCT_IDENTIFICATION;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->localPos();
#endif
}

static QPoint mouseGlobal(const QMouseEvent *event)
{
    FCT_IDENTIFICATION;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

static double mouseX(const QMouseEvent *event)
{
    FCT_IDENTIFICATION;

    return mousePoint(event).x();
}

PanadapterView::PanadapterView(QWidget *parent) :
    QWidget(parent)
{
    FCT_IDENTIFICATION;

    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::CrossCursor);
    buildPalette();
}

void PanadapterView::buildPalette()
{
    FCT_IDENTIFICATION;

    /* Noise in blues, signals in the warm end. */
    const QVector<QPair<double, QColor>> stops = {
        { 0.00, QColor(0, 0, 20) },
        { 0.18, QColor(0, 10, 120) },
        { 0.40, QColor(20, 90, 230) },
        { 0.60, QColor(40, 220, 230) },
        { 0.78, QColor(250, 240, 60) },
        { 0.92, QColor(250, 90, 30) },
        { 1.00, QColor(255, 255, 255) }
    };

    palette.resize(256);
    for ( int i = 0; i < 256; i++ )
    {
        const double t = i / 255.0;
        int s = 0;
        while ( s + 2 < stops.size() && t > stops[s + 1].first )
            s++;
        const double span = stops[s + 1].first - stops[s].first;
        const double f = span > 0 ? qBound(0.0, ( t - stops[s].first ) / span, 1.0) : 0.0;
        const QColor &a = stops[s].second, &b = stops[s + 1].second;
        palette[i] = qRgb(int(a.red() + f * ( b.red() - a.red() )),
                          int(a.green() + f * ( b.green() - a.green() )),
                          int(a.blue() + f * ( b.blue() - a.blue() )));
    }
}

int PanadapterView::pixels() const
{
    FCT_IDENTIFICATION;

    return qMax(1, int(std::lround(width() * devicePixelRatioF())));
}

QRect PanadapterView::spectrumArea() const
{
    FCT_IDENTIFICATION;

    return QRect(0, 0, width(), qMax(40, int(height() * SPECTRUM_SHARE)));
}

QRect PanadapterView::scaleArea() const
{
    FCT_IDENTIFICATION;

    return QRect(0, spectrumArea().bottom() + 1, width(), SCALE_HEIGHT);
}

QRect PanadapterView::waterfallArea() const
{
    FCT_IDENTIFICATION;

    const int top = scaleArea().bottom() + 1;
    return QRect(0, top, width(), qMax(1, height() - top));
}

QSize PanadapterView::waterfallPixels() const
{
    FCT_IDENTIFICATION;

    const QRect area = waterfallArea();
    return QSize(pixels(), qMax(1, int(std::lround(area.height() * devicePixelRatioF()))));
}

double PanadapterView::hzAt(double x) const
{
    FCT_IDENTIFICATION;

    return low + ( high - low ) * x / qMax(1, width());
}

double PanadapterView::xAt(double hz) const
{
    FCT_IDENTIFICATION;

    if ( high <= low )
        return -1;
    return ( hz - low ) * width() / ( high - low );
}

/* 6 dB under the floor, so noise is dark blue, not black. */
double PanadapterView::bottomLevel() const
{
    // no FCT_IDENTIFICATION: called once per pixel while drawing
    return floor - 6.0 + offset;
}

double PanadapterView::topLevel() const
{
    // no FCT_IDENTIFICATION: called once per pixel while drawing
    return bottomLevel() + range;
}

QRgb PanadapterView::colourFor(float db) const
{
    // no FCT_IDENTIFICATION: called once per pixel of every waterfall line
    if ( db <= NO_DATA )
        return palette[0];
    const double t = ( db - bottomLevel() ) / range;
    return palette[qBound(0, int(t * 255.0), 255)];
}

void PanadapterView::setWindow(double lowHz, double highHz)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << lowHz << highHz;

    if ( lowHz == low && highHz == high )
        return;

    const double oldWidth = high - low;
    const double newWidth = highHz - lowHz;
    const bool sameZoom = oldWidth > 0 && std::fabs(newWidth - oldWidth) < 1e-6 * oldWidth;

    if ( sameZoom && !waterfall.isNull() )
    {
        /* Shift by whole pixels only and keep low/high aligned to them,
           so sub-pixel steps do not accumulate into drift. */
        const double hzPerPixel = oldWidth / qMax(1, waterfall.width());
        const int dx = int(std::lround(( lowHz - low ) / hzPerPixel));

        if ( std::abs(dx) >= waterfall.width() )
            waterfall.fill(palette[0]);
        else if ( dx != 0 )
        {
            const int w = waterfall.width();
            for ( int y = 0; y < waterfall.height(); y++ )
            {
                QRgb *row = reinterpret_cast<QRgb *>(waterfall.scanLine(y));
                if ( dx > 0 )
                {
                    std::memmove(row, row + dx, size_t(w - dx) * sizeof(QRgb));
                    std::fill(row + w - dx, row + w, palette[0]);
                }
                else
                {
                    std::memmove(row - dx, row, size_t(w + dx) * sizeof(QRgb));
                    std::fill(row, row - dx, palette[0]);
                }
            }
        }
        low = low + dx * hzPerPixel;
        high = low + newWidth;
    }
    else
    {
        low = lowHz;
        high = highHz;
        if ( !waterfall.isNull() )
            waterfall.fill(palette[0]);
    }

    line.clear();
    update();
}

void PanadapterView::setTuning(double rxHz, double passLowHz, double passHighHz)
{
    FCT_IDENTIFICATION;

    if ( rxHz == rx && passLowHz == passLow && passHighHz == passHigh )
        return;
    rx = rxHz;
    passLow = passLowHz;
    passHigh = passHighHz;
    update();
}

void PanadapterView::setTransmitting(bool on)
{
    FCT_IDENTIFICATION;

    if ( transmitting == on )
        return;
    transmitting = on;
    update();
}

void PanadapterView::setMessage(const QString &text)
{
    FCT_IDENTIFICATION;

    if ( message == text )
        return;
    message = text;
    update();
}

void PanadapterView::setLevels(double rangeDb, double offsetDb)
{
    FCT_IDENTIFICATION;

    range = qBound(5.0, rangeDb, 120.0);
    offset = qBound(-40.0, offsetDb, 60.0);
    update();
}

void PanadapterView::setBands(const QVector<Segment> &allowed, const QVector<Segment> &segments)
{
    FCT_IDENTIFICATION;

    allowedBands = allowed;
    bandSegments = segments;
    update();
}

void PanadapterView::setPointerPassband(double lowOffsetHz, double highOffsetHz)
{
    FCT_IDENTIFICATION;

    pointerLow = lowOffsetHz;
    pointerHigh = highOffsetHz;
    update();
}

void PanadapterView::setSpots(const QVector<SpotMark> &marks)
{
    FCT_IDENTIFICATION;

    spotMarks = marks;
    spotRects.clear();
    update();
}

int PanadapterView::spotAt(const QPointF &point) const
{
    FCT_IDENTIFICATION;

    for ( int i = spotRects.size() - 1; i >= 0; i-- )
        if ( spotRects[i].contains(point) )
            return i;
    return -1;
}

void PanadapterView::clear()
{
    FCT_IDENTIFICATION;

    line.clear();
    floorKnown = false;
    if ( !waterfall.isNull() )
        waterfall.fill(palette[0]);
    update();
}

void PanadapterView::addSpectrum(const QVector<float> &decibels, double firstHz, double binHz)
{
    FCT_IDENTIFICATION;

    const int w = pixels();

    if ( width() <= 0 || high <= low || binHz <= 0 || decibels.isEmpty() )
        return;

    const int bins = decibels.size();
    const double hzPerPixel = ( high - low ) / w;
    line.resize(w);

    for ( int x = 0; x < w; x++ )
    {
        const double f0 = ( low + x * hzPerPixel - firstHz ) / binHz;
        const double f1 = f0 + hzPerPixel / binHz;

        if ( f1 - f0 < 1.0 )
        {
            /* Several pixels per bin: interpolate between bin centres. */
            const double at = ( f0 + f1 ) / 2.0 - 0.5;
            const int a = int(std::floor(at));
            const double t = at - a;
            if ( a < 0 || a + 1 >= bins )
                line[x] = NO_DATA;
            else
                line[x] = float(decibels[a] * ( 1.0 - t ) + decibels[a + 1] * t);
            continue;
        }

        const int a = qMax(int(std::floor(f0)), 0);
        const int b = qMin(int(std::ceil(f1)), bins);
        float strongest = NO_DATA;
        for ( int i = a; i < b; i++ )
            strongest = qMax(strongest, decibels[i]);
        line[x] = strongest;
    }

    QVector<float> present;
    present.reserve(w);
    for ( float v : static_cast<const QVector<float> &>(line) )
        if ( v > NO_DATA )
            present << v;

    /* Floor frozen during TX, so a muted receiver is visible as such. */
    if ( !present.isEmpty() && ( !transmitting || !floorKnown ) )
    {
        std::nth_element(present.begin(), present.begin() + present.size() / 2, present.end());
        const double median = present[present.size() / 2];
        floor = floorKnown ? floor + FLOOR_FOLLOW * ( median - floor ) : median;
        floorKnown = true;
    }

    if ( !transmitting )
    {
        const QSize size = waterfallPixels();

        if ( waterfall.size() != size )
        {
            waterfall = QImage(size, QImage::Format_RGB32);
            waterfall.fill(palette[0]);
        }

        const int rowBytes = waterfall.bytesPerLine();
        uchar *bits = waterfall.bits();
        std::memmove(bits + rowBytes, bits, size_t(rowBytes) * ( waterfall.height() - 1 ));
        QRgb *top = reinterpret_cast<QRgb *>(waterfall.scanLine(0));
        for ( int x = 0; x < qMin(w, waterfall.width()); x++ )
            top[x] = colourFor(line[x]);
    }

    update();
}

void PanadapterView::resizeEvent(QResizeEvent *event)
{
    FCT_IDENTIFICATION;

    QWidget::resizeEvent(event);

    if ( waterfall.isNull() )
        return;

    /* Keep history while the dock is resized. */
    const QSize size = waterfallPixels();
    QImage resized(size, QImage::Format_RGB32);
    resized.fill(palette[0]);
    if ( resized.width() == waterfall.width() )
    {
        QPainter p(&resized);
        p.drawImage(0, 0, waterfall);
    }
    waterfall = resized;
    line.clear();
}

void PanadapterView::paintEvent(QPaintEvent *)
{
    FCT_IDENTIFICATION;

    QPainter p(this);
    const QRect spec = spectrumArea();
    const QRect scale = scaleArea();
    const QRect fall = waterfallArea();
    const double dpr = devicePixelRatioF();

    p.fillRect(spec, QColor(8, 12, 30));
    p.fillRect(scale, QColor(20, 24, 40));

    if ( waterfall.isNull() )
        p.fillRect(fall, palette[0]);
    else
        p.drawImage(QRectF(fall.left(), fall.top(), waterfall.width() / dpr, waterfall.height() / dpr), waterfall);

    /* TX privileges tinted red; band plan as a strip on top. */
    for ( const Segment &band : static_cast<const QVector<Segment> &>(allowedBands) )
    {
        if ( band.high < low || band.low > high )
            continue;
        p.fillRect(QRectF(QPointF(xAt(band.low), scale.top()), QPointF(xAt(band.high), scale.bottom() + 1)),
                   band.colour);
    }
    for ( const Segment &segment : static_cast<const QVector<Segment> &>(bandSegments) )
    {
        if ( segment.high < low || segment.low > high )
            continue;
        p.fillRect(QRectF(QPointF(xAt(segment.low), scale.top()), QPointF(xAt(segment.high), scale.top() + BAND_STRIP)),
                   segment.colour);
    }

    /* Passband */
    if ( passHigh > passLow && high > low )
    {
        const double x0 = xAt(passLow), x1 = xAt(passHigh);
        p.fillRect(QRectF(QPointF(x0, spec.top()), QPointF(qMax(x0 + 1, x1), spec.bottom() + 1)), QColor(40, 160, 60, 70));
        p.setPen(QPen(QColor(60, 200, 80, 150), 1, Qt::DashLine));
        p.drawLine(QPointF(x0, fall.top()), QPointF(x0, fall.bottom()));
        p.drawLine(QPointF(x1, fall.top()), QPointF(x1, fall.bottom()));
    }

    /* Level grid, 10 dB */
    if ( floorKnown )
    {
        p.setFont(QFont(font().family(), 7));
        for ( int step = -10; step <= int(range + offset); step += 10 )
        {
            const double level = floor + step;
            if ( level < bottomLevel() || level > topLevel() )
                continue;
            const double y = spec.top() + spec.height() * ( topLevel() - level ) / range;
            p.setPen(QColor(50, 56, 90));
            p.drawLine(QPointF(0, y), QPointF(spec.width(), y));
            p.setPen(QColor(110, 120, 160));
            p.drawText(QPointF(3, y - 2), step == 0 ? tr("floor") : QStringLiteral("%1%2 dB").arg(step > 0 ? "+" : "").arg(step));
        }
    }

    /* Spectrum */
    if ( floorKnown && line.size() == pixels() )
    {
        QPainterPath path;
        path.moveTo(0, spec.bottom() + 1);
        for ( int x = 0; x < line.size(); x++ )
        {
            const double v = line[x] <= NO_DATA ? bottomLevel() : line[x];
            const double y = spec.top() + spec.height() * ( topLevel() - v ) / range;
            path.lineTo(x / dpr, qBound(double(spec.top()), y, double(spec.bottom() + 1)));
        }
        path.lineTo(( line.size() - 1 ) / dpr, spec.bottom() + 1);
        path.closeSubpath();
        p.save();
        p.setClipRect(spec);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillPath(path, QColor(60, 110, 200, 110));
        p.setPen(QPen(QColor(150, 200, 255), 1.0 / dpr));
        p.drawPath(path);
        p.restore();
    }

    /* Frequency scale: 1-2-5 steps, ~100 px between labels. */
    if ( high > low )
    {
        const double perLabel = ( high - low ) * 100.0 / qMax(1, width());
        const double series[] = { 1, 2, 5 };
        double step = 10.0;
        bool found = false;
        for ( double decade = 10.0; decade < 1e8 && !found; decade *= 10.0 )
            for ( double s : series )
                if ( s * decade >= perLabel ) { step = s * decade; found = true; break; }
        const int decimals = step >= 1000.0 ? 3 : ( step >= 100.0 ? 4 : 5 );

        p.setFont(QFont(font().family(), 7));
        for ( double f = std::ceil(low / step) * step; f <= high; f += step )
        {
            const double x = xAt(f);
            p.setPen(QColor(150, 160, 200));
            p.drawLine(QPointF(x, scale.top() + BAND_STRIP), QPointF(x, scale.top() + BAND_STRIP + 4));
            p.setPen(QColor(40, 46, 80));
            p.drawLine(QPointF(x, spec.top()), QPointF(x, spec.bottom()));
            p.setPen(QColor(220, 225, 245));
            const QString label = QString::number(f / 1e6, 'f', decimals);
            const int textWidth = p.fontMetrics().horizontalAdvance(label);
            p.drawText(QPointF(x - textWidth / 2.0, scale.bottom() - 2), label);
        }
    }

    /* Spots: labels in rows; one that does not fit is skipped, its line stays. */
    spotRects = QVector<QRectF>(spotMarks.size());
    if ( !spotMarks.isEmpty() && high > low )
    {
        QFont spotFont(font().family(), 8, QFont::Bold);
        p.setFont(spotFont);
        const QFontMetricsF metrics(spotFont);
        const double rowHeight = metrics.height() + 1;
        const int rows = qMax(1, qMin(4, int(( spec.height() - 8 ) / rowHeight)));
        QVector<double> rowEnd(rows, -1e9);

        QVector<int> order(spotMarks.size());
        for ( int i = 0; i < order.size(); i++ )
            order[i] = i;
        std::sort(order.begin(), order.end(), [this](int a, int b) { return spotMarks[a].hz < spotMarks[b].hz; });

        for ( int i : order )
        {
            const SpotMark &mark = spotMarks[i];
            if ( mark.hz < low || mark.hz > high )
                continue;

            const double x = xAt(mark.hz);
            const double w = metrics.horizontalAdvance(mark.label) + 6;
            const double left = x - w / 2;
            int row = -1;
            for ( int r = 0; r < rows; r++ )
                if ( rowEnd[r] + 3 <= left ) { row = r; break; }

            QColor line = mark.colour;
            line.setAlpha(110);
            p.setPen(QPen(line, 1, Qt::DotLine));

            if ( row < 0 )
            {
                p.drawLine(QPointF(x, spec.top() + rows * rowHeight), QPointF(x, spec.bottom()));
            }
            else
            {
                rowEnd[row] = left + w;
                const QRectF box(left, spec.top() + 2 + row * rowHeight, w, rowHeight - 1);
                spotRects[i] = box;
                p.drawLine(QPointF(x, box.bottom()), QPointF(x, spec.bottom()));
                p.fillRect(box, QColor(0, 0, 0, 170));
                p.setPen(mark.colour);
                p.drawText(box, Qt::AlignCenter, mark.label);
            }

            p.fillRect(QRectF(x - 1, scale.top(), 2, BAND_STRIP + 3), mark.colour);
        }
    }

    /* RX marker */
    if ( rx > 0 && rx >= low && rx <= high )
    {
        const double x = xAt(rx);
        p.setPen(QPen(QColor(255, 60, 60), 1));
        p.drawLine(QPointF(x, spec.top()), QPointF(x, fall.bottom()));
    }

    if ( hoverX >= 0 && high > low )
    {
        const double f = hzAt(hoverX);

        if ( pointerHigh > pointerLow )
        {
            const double x0 = xAt(f + pointerLow), x1 = xAt(f + pointerHigh);
            p.fillRect(QRectF(QPointF(x0, spec.top()), QPointF(qMax(x0 + 1, x1), spec.bottom() + 1)),
                       QColor(255, 255, 255, 45));
            p.fillRect(QRectF(QPointF(x0, fall.top()), QPointF(qMax(x0 + 1, x1), fall.bottom() + 1)),
                       QColor(255, 255, 255, 30));
            p.setPen(QPen(QColor(255, 255, 255, 110), 1));
            p.drawLine(QPointF(x0, spec.top()), QPointF(x0, fall.bottom()));
            p.drawLine(QPointF(x1, spec.top()), QPointF(x1, fall.bottom()));
        }

        p.setPen(QPen(QColor(255, 255, 255, 120), 1, Qt::DotLine));
        p.drawLine(hoverX, spec.top(), hoverX, fall.bottom());
        const QString label = QString::number(f / 1e6, 'f', 4) + QStringLiteral(" MHz");
        p.setFont(QFont(font().family(), 8));
        const int tw = p.fontMetrics().horizontalAdvance(label) + 8;
        const int tx = hoverX + tw + 6 < width() ? hoverX + 6 : hoverX - tw - 6;
        p.fillRect(QRect(tx, spec.top() + 4, tw, 16), QColor(0, 0, 0, 170));
        p.setPen(Qt::white);
        p.drawText(QRect(tx, spec.top() + 4, tw, 16), Qt::AlignCenter, label);
    }

    if ( transmitting )
    {
        p.setFont(QFont(font().family(), 14, QFont::Bold));
        p.setPen(QColor(255, 80, 80));
        p.drawText(spec.adjusted(0, 6, -10, 0), Qt::AlignRight | Qt::AlignTop, tr("TX"));
    }

    if ( !message.isEmpty() )
    {
        p.setFont(QFont(font().family(), 10));
        p.setPen(QColor(230, 230, 240));
        p.drawText(spec, Qt::AlignCenter | Qt::TextWordWrap, message);
    }
}

void PanadapterView::mousePressEvent(QMouseEvent *event)
{
    FCT_IDENTIFICATION;

    if ( event->button() == Qt::LeftButton )
    {
        pressed = true;
        dragging = false;
        pressPoint = mousePoint(event);
        lastDragX = pressPoint.x();
        pressSpot = spotAt(pressPoint);
    }
    QWidget::mousePressEvent(event);
}

void PanadapterView::mouseReleaseEvent(QMouseEvent *event)
{
    FCT_IDENTIFICATION;

    if ( event->button() == Qt::LeftButton && pressed )
    {
        if ( !dragging )
        {
            if ( pressSpot >= 0 )
                emit spotClicked(pressSpot);
            else if ( high > low )
                emit clicked(hzAt(pressPoint.x()));
        }
        pressed = false;
        dragging = false;
        setCursor(spotAt(mousePoint(event)) >= 0 ? Qt::PointingHandCursor : Qt::CrossCursor);
    }
    QWidget::mouseReleaseEvent(event);
}

void PanadapterView::mouseDoubleClickEvent(QMouseEvent *event)
{
    FCT_IDENTIFICATION;

    if ( event->button() == Qt::LeftButton && spotAt(mousePoint(event)) < 0 )
        emit doubleClicked();
    QWidget::mouseDoubleClickEvent(event);
}

void PanadapterView::mouseMoveEvent(QMouseEvent *event)
{
    FCT_IDENTIFICATION;

    hoverX = int(mouseX(event));

    if ( pressed && ( event->buttons() & Qt::LeftButton ) )
    {
        const double x = mouseX(event);

        if ( !dragging && std::fabs(x - pressPoint.x()) > DRAG_THRESHOLD )
        {
            dragging = true;
            QToolTip::hideText();
            setCursor(Qt::ClosedHandCursor);
        }
        if ( dragging )
        {
            /* Drag right shows lower frequencies. */
            const double hz = ( lastDragX - x ) * ( high - low ) / qMax(1, width());
            lastDragX = x;
            if ( hz != 0.0 )
                emit dragged(hz);
            update();
            QWidget::mouseMoveEvent(event);
            return;
        }
    }

    const int spot = spotAt(mousePoint(event));
    if ( spot >= 0 )
    {
        QToolTip::showText(mouseGlobal(event), spotMarks[spot].tip, this);
        setCursor(Qt::PointingHandCursor);
    }
    else
    {
        QToolTip::hideText();
        setCursor(Qt::CrossCursor);
    }
    update();
    QWidget::mouseMoveEvent(event);
}

void PanadapterView::wheelEvent(QWheelEvent *event)
{
    FCT_IDENTIFICATION;

    /* One notch = 120; touchpads send fractions, so accumulate. */
    wheelPending += event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
    const int steps = wheelPending / 120;
    wheelPending -= steps * 120;
    if ( steps )
        emit wheelTurned(steps, event->modifiers());
    event->accept();
}

void PanadapterView::leaveEvent(QEvent *event)
{
    FCT_IDENTIFICATION;

    hoverX = -1;
    update();
    QWidget::leaveEvent(event);
}
