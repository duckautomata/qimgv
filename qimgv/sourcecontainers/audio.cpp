#include "audio.h"

Audio::Audio(QString path) : Image(path) {
    Audio::load();
}

Audio::Audio(std::unique_ptr<DocumentInfo> info) : Image(std::move(info)) {
    Audio::load();
}

void Audio::load() {
    mLoaded = true;
}

std::unique_ptr<QPixmap> Audio::getPixmap() {
    return nullptr;
}

std::shared_ptr<const QImage> Audio::getImage() {
    return nullptr;
}

int Audio::height() {
    return 0;
}

int Audio::width() {
    return 0;
}

QSize Audio::size() {
    return QSize(0, 0);
}

bool Audio::save() {
    return false;
}

bool Audio::save(QString destPath) {
    Q_UNUSED(destPath)
    return false;
}
