#include "thumbnail.h"

#include <QApplication>

Thumbnail::Thumbnail(QString _name, QString _info, int _size, std::shared_ptr<QImage> _image)
    : mName(_name), mInfo(_info), mImage(_image), mSize(_size), mHasAlphaChannel(false) {
    if(_image)
        mHasAlphaChannel = _image->hasAlphaChannel();
}

QString Thumbnail::name() {
    return mName;
}

QString Thumbnail::info() {
    return mInfo;
}

int Thumbnail::size() {
    return mSize;
}

bool Thumbnail::hasAlphaChannel() {
    return mHasAlphaChannel;
}

std::shared_ptr<QPixmap> Thumbnail::pixmap() {
    if(!mPixmap) {
        // Never null, even for a thumbnail that failed to decode: callers test
        // the result with width() == 0 rather than for a null pointer.
        mPixmap = mImage ? std::make_shared<QPixmap>(QPixmap::fromImage(*mImage)) : std::make_shared<QPixmap>();
        // Read here rather than on the worker, so it is both the right thread
        // and the current value if the display changed since decoding.
        mPixmap->setDevicePixelRatio(qApp->devicePixelRatio());
    }
    return mPixmap;
}
