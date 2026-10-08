#pragma once

#include <QColor>
#include <QDebug>
#include <QPixmap>
#include <map>
#include <tuple>

enum ShrIcon { SHR_ICON_ERROR, SHR_ICON_LOADING, SHR_ICON_AUDIO };

// The placeholder icons thumbnails draw in place of an image. GUI thread only:
// these are QPixmaps.
class SharedResources {
public:
    static SharedResources *getInstance();

    // `icon` for a display at `dpr`, filled with `color`. Each combination is
    // loaded and recoloured once instead of on every paint; a window can span
    // screens with different scale factors, so one icon may be needed at more
    // than one dpr at a time.
    QPixmap getPixmap(ShrIcon icon, qreal dpr, QColor const &color);

private:
    std::map<std::tuple<ShrIcon, qreal, QRgb>, QPixmap> mPixmaps;
};

extern SharedResources *shrRes;
