#pragma once

#include <QImage>
#include <QPixmap>
#include <QString>
#include <memory>

// Holds the decoded thumbnail as a QImage and only turns it into a QPixmap when
// something asks to draw it.
//
// That is not laziness for its own sake: thumbnails are decoded on a thread
// pool, and QPixmap may only be touched on the GUI thread. Constructing one on a
// worker is undefined behaviour that mostly appears to work, which is the worst
// kind. QImage has no such restriction, so the worker produces one of those and
// the conversion happens on whichever thread paints -- always the GUI thread.
class Thumbnail {
public:
    Thumbnail(QString _name, QString _info, int _size, std::shared_ptr<QImage> _image);
    QString name();
    QString info();
    int size();
    bool hasAlphaChannel();
    // Call from the GUI thread only.
    std::shared_ptr<QPixmap> pixmap();

private:
    QString mName, mInfo;
    std::shared_ptr<QImage> mImage;
    std::shared_ptr<QPixmap> mPixmap; // built on first use
    int mSize;
    bool mHasAlphaChannel;
};
