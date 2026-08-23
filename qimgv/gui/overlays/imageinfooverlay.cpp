#include "imageinfooverlay.h"
#include "ui_imageinfooverlay.h"

#include <QApplication>
#include <QPainter>
#include <QScrollBar>

ElidingLabel::ElidingLabel(QWidget *parent) : QLabel(parent) {}

void ElidingLabel::setFullText(QString const &text) {
    full = text;
    setToolTip(text);
    update();
}

// Zero, so a long value can never widen the panel -- the elide handles the rest.
QSize ElidingLabel::minimumSizeHint() const {
    return {0, QLabel::minimumSizeHint().height()};
}

void ElidingLabel::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setPen(palette().color(foregroundRole()));
    painter.drawText(rect(), int(alignment()), fontMetrics().elidedText(full, Qt::ElideRight, width()));
}

ImageInfoOverlay::ImageInfoOverlay(FloatingWidgetContainer *parent)
    : OverlayWidget(parent), ui(new Ui::ImageInfoOverlay) {
    ui->setupUi(this);
    ui->closeButton->setIconPath(":res/icons/common/overlay/close-dim16.png");
    ui->headerIcon->setIconPath(":res/icons/common/overlay/info16.png");
    connect(ui->closeButton, &IconButton::clicked, this, &ImageInfoOverlay::hide);
    setPosition(FloatingWidgetPosition::RIGHT);

    content = new QWidget(this);
    contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    contentLayout->addStretch(1);

    statusLabel = new QLabel(content);
    statusLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    statusLabel->setContentsMargins(8, 12, 8, 12);
    contentLayout->insertWidget(0, statusLabel);

    scrollArea = new QScrollArea(this);
    scrollArea->setWidget(content);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->viewport()->setAutoFillBackground(false);
    content->setAutoFillBackground(false);
    ui->entryLayout->addWidget(scrollArea);

    setStatus(tr("No file open"));

    if(parent)
        setContainerSize(parent->size());
}

ImageInfoOverlay::~ImageInfoOverlay() {
    delete ui;
}

void ImageInfoOverlay::setStatus(QString const &text) {
    statusLabel->setText(text);
    statusLabel->setVisible(!text.isEmpty());
}

// Rows are reused across images. Only the count changes here; the text is set
// by the caller, which is the cheap part.
void ImageInfoOverlay::buildRows(int count) {
    while(rows.count() > count) {
        Row r = rows.takeLast();
        contentLayout->removeWidget(r.widget);
        delete r.widget;
    }
    while(rows.count() < count) {
        Row r;
        r.widget = new QWidget(content);
        auto *l = new QHBoxLayout(r.widget);
        l->setContentsMargins(8, 1, 8, 1);
        l->setSpacing(8);
        r.name = new QLabel(r.widget);
        r.value = new ElidingLabel(r.widget);
        r.name->setMinimumWidth(96);
        r.name->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        r.value->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        l->addWidget(r.name, 0);
        l->addWidget(r.value, 1);
        // Ahead of the trailing stretch.
        contentLayout->insertWidget(contentLayout->count() - 1, r.widget);
        rows.append(r);
    }
}

void ImageInfoOverlay::setLoading() {
    buildRows(0);
    setStatus(tr("Reading..."));
    if(!isHidden())
        recalculateGeometry();
}

void ImageInfoOverlay::setInfo(QVector<FileInfoSection> const &sections) {
    int needed = 0;
    for(auto const &s : sections)
        needed += 1 + s.fields.count(); // a heading, then its fields

    if(!needed) {
        buildRows(0);
        setStatus(tr("No metadata found"));
        return;
    }
    setStatus(QString());
    buildRows(needed);

    int i = 0;
    for(auto const &section : sections) {
        Row const &heading = rows.at(i++);
        heading.name->setText(section.title);
        QFont headingFont = heading.name->font();
        headingFont.setBold(true);
        heading.name->setFont(headingFont);
        heading.widget->setContentsMargins(0, 6, 0, 2);
        heading.value->setFullText(QString());
        for(auto const &field : section.fields) {
            Row const &row = rows.at(i++);
            QFont fieldFont = row.name->font();
            fieldFont.setBold(false);
            row.name->setFont(fieldFont);
            row.widget->setContentsMargins(0, 0, 0, 0);
            row.name->setText(field.name);
            row.value->setFullText(field.value);
        }
    }
    scrollArea->verticalScrollBar()->setValue(0);
    // Rows were just inserted; without this the hint above is computed against
    // the previous contents.
    contentLayout->activate();
    if(!isHidden())
        recalculateGeometry();
}

// recalculateGeometry() builds its rect from sizeHint() and calls setGeometry,
// so this -- not resize() -- is where the panel's size is decided. Anything
// resize() sets is overwritten on the next reposition.
//
// The clamp matters: recalculateGeometry() centres the widget vertically
// without checking it fits, so a panel taller than the viewer would push its
// own header off the top with no way to reach it.
QSize ImageInfoOverlay::sizeHint() const {
    // setupUi() lays out, and laying out asks for sizeHint, so this runs once
    // before the scroll area exists.
    if(!content || !contentLayout || !ui->header)
        return OverlayWidget::sizeHint();
    QSize const box = const_cast<ImageInfoOverlay *>(this)->containerSize();
    // The layout's own hint, not the widget's: QWidget::sizeHint() is cached and
    // is still the pre-insert value immediately after rows are added, which
    // sized the panel to a fraction of its content.
    int const wanted = ui->header->sizeHint().height() + contentLayout->totalSizeHint().height() + 8;
    if(!box.isValid() || box.isEmpty())
        return {qMax(minimumWidth(), 360), qMax(80, wanted)};
    int const width = qMax(minimumWidth(), qMin(560, int(box.width() * 0.6)));
    // Reads better with room to breathe: fill most of the viewer when there is
    // enough to show, rather than sizing tightly to a handful of rows.
    int const maxHeight = qMax(200, int(box.height() * 0.92));
    int const height = qBound(qMin(200, maxHeight), wanted, maxHeight);
    return {width, height};
}

void ImageInfoOverlay::show() {
    OverlayWidget::show();
    recalculateGeometry();
}

void ImageInfoOverlay::wheelEvent(QWheelEvent *event) {
    // The scroll area consumes what it can use; this catches the leftovers at
    // either end so the wheel never reaches the viewer and changes the image.
    event->accept();
}
