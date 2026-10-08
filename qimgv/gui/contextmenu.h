#pragma once

#include <QScreen>
#include <QStyleOption>
#include <QTimer>
#include <QPainter>
#include "components/actionmanager/actionmanager.h"
#include "components/scriptmanager/scriptmanager.h"

namespace Ui {
class ContextMenu;
}

class ContextMenu : public QWidget {
    Q_OBJECT
public:
    explicit ContextMenu(QWidget *parent = nullptr);
    ~ContextMenu();
    // Rotate, flip, crop, resize and print: the entries that work on pixels.
    void setImageEntriesEnabled(bool mode);
    // Copy, move, trash, open with and show in folder: they act on the file, whatever is in it.
    void setFileEntriesEnabled(bool mode);

public slots:
    void showAt(QPoint pos);
    void setGeometry(QRect geom);

private:
    Ui::ContextMenu *ui;
    QTimer mTimer;

    void fillOpenWithMenu();

signals:
    void showScriptSettings();

protected:
    void mousePressEvent(QMouseEvent *);
    void paintEvent(QPaintEvent *);
    void keyPressEvent(QKeyEvent *event);
private slots:
    void switchToMainPage();
    void switchToScriptsPage();
};
