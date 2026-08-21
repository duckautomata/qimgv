#pragma once

#include <QObject>
#include <QSize>
#include <QSizeF>

class QWidget;
class MapOverlay;

class Test_MapOverlay : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void outerFitsMapPreservingAspect();
    void innerMatchesScaledWindowArea();
    void innerNeverExceedsOuter();

private:
    const QSize kWindowSize{200, 100};
    const QSizeF kDrawingSize{1400, 1200};

    QWidget *parent = nullptr;
    MapOverlay *minimap = nullptr;
};
