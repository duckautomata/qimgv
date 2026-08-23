#include "imageinfooverlayproxy.h"

ImageInfoOverlayProxy::ImageInfoOverlayProxy(FloatingWidgetContainer *parent) : container(parent), overlay(nullptr) {}

ImageInfoOverlayProxy::~ImageInfoOverlayProxy() {
    if(overlay)
        overlay->deleteLater();
}

void ImageInfoOverlayProxy::show() {
    init();
    overlay->show();
}

void ImageInfoOverlayProxy::hide() {
    if(overlay)
        overlay->hide();
}

void ImageInfoOverlayProxy::init() {
    if(overlay)
        return;
    overlay = new ImageInfoOverlay(container);
    if(stateBuf.loading)
        overlay->setLoading();
    else
        overlay->setInfo(stateBuf.sections);
}

bool ImageInfoOverlayProxy::isHidden() {
    return overlay ? overlay->isHidden() : true;
}

void ImageInfoOverlayProxy::setInfo(QVector<FileInfoSection> const &sections) {
    stateBuf.loading = false;
    if(overlay)
        overlay->setInfo(sections);
    else
        stateBuf.sections = sections;
}

void ImageInfoOverlayProxy::setLoading() {
    stateBuf.loading = true;
    stateBuf.sections.clear();
    if(overlay)
        overlay->setLoading();
}
