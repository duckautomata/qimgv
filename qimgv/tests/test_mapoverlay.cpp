#include "test_mapoverlay.h"

#include <QtTest>
#include "gui/overlays/mapoverlay.h"

namespace {
// MapOverlay computes its geometry in float but exposes QSizeF (double), so
// the low bits never match a double-precision expectation exactly.
constexpr qreal kEpsilon = 1e-4;

bool sizesAreClose(QSizeF a, QSizeF b) {
    return qAbs(a.width() - b.width()) < kEpsilon
        && qAbs(a.height() - b.height()) < kEpsilon;
}
} // namespace

// Reports both sizes on failure, unlike a bare QVERIFY.
#define COMPARE_SIZES(actual, expected)                                        \
    QVERIFY2(sizesAreClose((actual), (expected)),                              \
             qPrintable(QStringLiteral("actual %1x%2 != expected %3x%4")       \
                            .arg((actual).width()).arg((actual).height())      \
                            .arg((expected).width()).arg((expected).height())))

void Test_MapOverlay::initTestCase() {
    parent = new QWidget();
    parent->resize(kWindowSize);

    minimap = new MapOverlay(parent);
    minimap->updateMap(QRectF(QPointF(0, 0), kDrawingSize));
}

void Test_MapOverlay::cleanupTestCase() {
    delete parent; // owns minimap
    parent = nullptr;
    minimap = nullptr;
}

// The drawing area is scaled down to fit a square of minimap->size(),
// preserving aspect ratio. The longest side therefore lands exactly on
// the map size.
void Test_MapOverlay::outerFitsMapPreservingAspect() {
    const qreal mapSize = minimap->size();
    const qreal scale = kDrawingSize.width() / mapSize; // width is the long side here
    const QSizeF expected(kDrawingSize.width() / scale, kDrawingSize.height() / scale);

    COMPARE_SIZES(minimap->outer(), expected);
    QVERIFY(qAbs(minimap->outer().width() - mapSize) < kEpsilon);
}

// The inner rect represents the visible window, expressed in the same scale
// as the outer rect.
void Test_MapOverlay::innerMatchesScaledWindowArea() {
    const qreal scale = kDrawingSize.width() / qreal(minimap->size());
    const QSizeF expected(kWindowSize.width() / scale, kWindowSize.height() / scale);

    COMPARE_SIZES(minimap->inner(), expected);
}

// The inner rect can never escape the outer rect.
void Test_MapOverlay::innerNeverExceedsOuter() {
    QVERIFY(minimap->inner().width()  <= minimap->outer().width());
    QVERIFY(minimap->inner().height() <= minimap->outer().height());
}

QTEST_MAIN(Test_MapOverlay)
