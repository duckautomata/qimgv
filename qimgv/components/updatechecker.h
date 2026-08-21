#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QVersionNumber>

// Asks GitHub whether a newer release exists. Deliberately narrow:
//
//   - It only ever reads. Nothing is downloaded, nothing is installed. A newer
//     version opens the releases page and the user takes it from there.
//     Self-updating would mean elevating, replacing a running executable and
//     verifying what came back -- a lot of attack surface for little gain.
//   - It is never required. Every failure path is silent apart from a string on
//     the About page: no dialogs, and nothing that can delay or block startup.
//   - The automatic check is opt-in and off by default, because a local image
//     viewer quietly contacting a server on every launch is not something a
//     user should have to discover.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // Asks now, regardless of the setting or when the last check ran.
    void check();
    // Asks only if the user enabled automatic checks and the interval elapsed.
    void checkIfDue();

    static QString releasesUrl();
    // "v2.0.1" -> 2.0.1, and "v2.1.0-rc1" -> 2.1.0. Public so it can be tested
    // without standing up a network.
    static QVersionNumber versionFromTag(QString tag);

signals:
    void updateAvailable(QVersionNumber version, QString url);
    void upToDate();
    void checkFailed(QString reason);

private:
    void handleReply(class QNetworkReply *reply);

    QNetworkAccessManager network;
    bool inFlight = false;
};
