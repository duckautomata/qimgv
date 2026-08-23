#pragma once

#include "components/fileinfo/fileinfoextractor.h"
#include "gui/customwidgets/overlaywidget.h"
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWheelEvent>

// QLabel neither elides nor wraps usefully here: word wrap refuses to break
// inside an unbroken token, so a long path or a 400-character XMP value just
// clips or forces the panel wider. This elides to the width it actually has and
// keeps the whole value in the tooltip.
class ElidingLabel : public QLabel {
public:
    explicit ElidingLabel(QWidget *parent = nullptr);
    void setFullText(QString const &text);
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString full;
};

namespace Ui {
class ImageInfoOverlay;
}

// The panel behind the I key. Shows what the file actually is -- name, size,
// real format, codec -- followed by every Exif, IPTC and XMP tag the file
// carries.
//
// Rows are rebuilt only when the shape of the data changes and their text is
// updated in place otherwise: building 150 rows costs ~30 ms, updating 150
// existing ones ~1 ms, and this is on the GUI thread.
class ImageInfoOverlay : public OverlayWidget {
    Q_OBJECT

public:
    explicit ImageInfoOverlay(FloatingWidgetContainer *parent = nullptr);
    ~ImageInfoOverlay() override;

    void setInfo(QVector<FileInfoSection> const &sections);
    // Shown while extraction is in flight, so the panel is never silently empty.
    void setLoading();

public slots:
    void show();

protected:
    void wheelEvent(QWheelEvent *event) override;
    QSize sizeHint() const override;

private:
    struct Row {
        QWidget *widget = nullptr;
        QLabel *name = nullptr;
        ElidingLabel *value = nullptr;
    };

    void setStatus(QString const &text);
    void buildRows(int count);

    Ui::ImageInfoOverlay *ui;
    QScrollArea *scrollArea = nullptr;
    QWidget *content = nullptr;
    QVBoxLayout *contentLayout = nullptr;
    QLabel *statusLabel = nullptr;
    QVector<Row> rows;
};
