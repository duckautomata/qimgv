#include "sharedresources.h"
#include "utils/imagelib.h"

SharedResources *shrRes = nullptr;

// No default case: an icon added to the enum without a path here is a compiler
// warning, not a silent fallback to one of the others.
static QString iconPath(ShrIcon icon) {
    switch(icon) {
    case SHR_ICON_ERROR:
        return QStringLiteral(":/res/icons/common/other/loading-error72.png");
    case SHR_ICON_LOADING:
        return QStringLiteral(":/res/icons/common/other/loading72.png");
    case SHR_ICON_AUDIO:
        return QStringLiteral(":/res/icons/common/other/audio-file72.png");
    }
    return {};
}

QPixmap SharedResources::getPixmap(ShrIcon icon, qreal dpr, QColor const &color) {
    auto const key = std::make_tuple(icon, dpr, color.rgba());
    auto const cached = mPixmaps.find(key);
    if(cached != mPixmaps.end())
        return cached->second;

    QString path = iconPath(icon);
    QPixmap pixmap;
    if(dpr >= (1.0 + 0.001)) {
        path.replace(".", "@2x.");
        pixmap.load(path);
        qreal pixmapDrawScale;
        if(dpr >= (2.0 - 0.001))
            pixmapDrawScale = dpr;
        else
            pixmapDrawScale = 2.0;
        pixmap.setDevicePixelRatio(pixmapDrawScale);
    } else {
        pixmap.load(path);
    }
    if(!pixmap.isNull())
        ImageLib::recolor(pixmap, color);

    // Each theme change leaves behind entries for colours nothing paints with
    // any more. There are only a handful of icons, so start over rather than
    // work out which ones are stale.
    if(mPixmaps.size() >= 32)
        mPixmaps.clear();
    mPixmaps.emplace(key, pixmap);
    return pixmap;
}

SharedResources *SharedResources::getInstance() {
    if(!shrRes) {
        shrRes = new SharedResources();
    }
    return shrRes;
}
