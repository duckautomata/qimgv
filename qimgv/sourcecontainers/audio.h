#pragma once

#include "image.h"

// An audio file. Like Video it carries no pixels: the player plugin reads the file itself, and the audio
// view fetches tags and cover art on its own. It exists so an audio file can travel through the directory
// model, the cache and the preloader like any other document.
class Audio : public Image {
public:
    Audio(QString path);
    Audio(std::unique_ptr<DocumentInfo> info);

    std::unique_ptr<QPixmap> getPixmap() override;
    std::shared_ptr<const QImage> getImage() override;
    int height() override;
    int width() override;
    QSize size() override;
    bool save() override;
    bool save(QString destPath) override;

private:
    void load() override;
};
