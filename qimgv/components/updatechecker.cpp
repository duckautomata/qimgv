#include "updatechecker.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QDebug>

#include "appversion.h"
#include "settings.h"

namespace {

// The unauthenticated API allows 60 requests an hour per address, which a
// once-a-day check plus the occasional manual one stays far below.
const char *kLatestReleaseApi = "https://api.github.com/repos/duckautomata/qimgv/releases/latest";
const char *kReleasesPage = "https://github.com/duckautomata/qimgv/releases/latest";

} // namespace

// Tags are written v2.0.1; the release job enforces that they match
// project(VERSION), so the numeric part is what we compare against appVersion.
QVersionNumber UpdateChecker::versionFromTag(QString tag) {
    tag = tag.trimmed();
    if(tag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        tag.remove(0, 1);
    int suffix = tag.indexOf(QLatin1Char('-')); // -rc1, -beta ...
    if(suffix != -1)
        tag.truncate(suffix);
    return QVersionNumber::fromString(tag);
}

QString UpdateChecker::releasesUrl() {
    return QString::fromLatin1(kReleasesPage);
}

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent) {
    connect(&network, &QNetworkAccessManager::finished, this, &UpdateChecker::handleReply);
}

void UpdateChecker::checkIfDue() {
    if(!settings->checkForUpdates())
        return;
    QDateTime const last = settings->lastUpdateCheck();
    if(last.isValid() && last.secsTo(QDateTime::currentDateTimeUtc()) < 24 * 60 * 60)
        return;
    check();
}

void UpdateChecker::check() {
    if(inFlight)
        return;
    inFlight = true;

    QNetworkRequest request{QUrl(QString::fromLatin1(kLatestReleaseApi))};
    request.setRawHeader("Accept", "application/vnd.github+json");
    // GitHub rejects requests without one.
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qimgv/%1").arg(appVersion.toString()));
    // Without this a stalled connection would leave the check pending forever.
    request.setTransferTimeout(10000);
    network.get(request);
}

void UpdateChecker::handleReply(QNetworkReply *reply) {
    reply->deleteLater();
    inFlight = false;

    // Record the attempt either way, so a machine that is simply offline does
    // not retry on every single launch.
    settings->setLastUpdateCheck(QDateTime::currentDateTimeUtc());

    if(reply->error() != QNetworkReply::NoError) {
        qDebug() << "[update] check failed:" << reply->errorString();
        // A repository with no published releases answers 404, which is not an
        // error worth showing as one -- say what it actually means.
        int const status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        emit checkFailed(status == 404 ? tr("No releases have been published yet") : reply->errorString());
        return;
    }

    QJsonParseError parseError{};
    QJsonDocument const doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
    if(parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        emit checkFailed(tr("Unexpected response from the update server"));
        return;
    }

    QVersionNumber const latest = UpdateChecker::versionFromTag(doc.object().value("tag_name").toString());
    if(latest.isNull()) {
        emit checkFailed(tr("Unexpected response from the update server"));
        return;
    }

    if(latest > appVersion)
        emit updateAvailable(latest, releasesUrl());
    else
        emit upToDate();
}
