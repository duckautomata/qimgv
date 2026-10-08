#pragma once

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QDebug>
#include "utils/inputmap.h"

class ShortcutBuilder {
public:
    static QString fromEvent(QInputEvent *event);
    // Play/pause, stop, next, previous and the volume keys: named from the key rather than the scan code.
    static bool isMediaKey(Qt::Key key);

private:
    static QString processWheelEvent(QWheelEvent *event);
    static QString processMouseEvent(QMouseEvent *event);
    static QString processKeyEvent(QKeyEvent *event);
    static QString modifierKeys(QInputEvent *event);
    static bool isModifier(Qt::Key key);
    static QString fromEventText(QKeyEvent *event);
    static QString fromEventNativeScanCode(QKeyEvent *event);
};
