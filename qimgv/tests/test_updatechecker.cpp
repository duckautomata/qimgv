// The update check decides whether to nag the user purely from a release tag,
// so the tag -> version step is the part worth pinning down. Everything else in
// UpdateChecker is network I/O.
#include <QtTest>
#include <QVersionNumber>

#include "components/updatechecker.h"

class Test_UpdateChecker : public QObject {
    Q_OBJECT

private slots:
    void parsesTags_data();
    void parsesTags();
    void comparesVersions_data();
    void comparesVersions();
    void releasesUrlPointsAtThisFork();
};

void Test_UpdateChecker::parsesTags_data() {
    QTest::addColumn<QString>("tag");
    QTest::addColumn<QVersionNumber>("expected");

    QTest::newRow("plain") << "v2.0.1" << QVersionNumber(2, 0, 1);
    QTest::newRow("no v prefix") << "2.0.1" << QVersionNumber(2, 0, 1);
    QTest::newRow("uppercase V") << "V2.0.1" << QVersionNumber(2, 0, 1);
    QTest::newRow("surrounding space") << "  v2.0.1  " << QVersionNumber(2, 0, 1);
    QTest::newRow("two components") << "v2.1" << QVersionNumber(2, 1);
    // A prerelease must compare as its base version, otherwise 2.1.0-rc1 would
    // parse as null and be silently ignored.
    QTest::newRow("rc suffix") << "v2.1.0-rc1" << QVersionNumber(2, 1, 0);
    QTest::newRow("beta suffix") << "v3.0.0-beta.2" << QVersionNumber(3, 0, 0);
    // Upstream's tags, since someone will inevitably point this at that repo.
    QTest::newRow("upstream tag") << "v1.0.2" << QVersionNumber(1, 0, 2);

    QTest::newRow("garbage") << "not-a-tag" << QVersionNumber();
    QTest::newRow("empty") << "" << QVersionNumber();
}

void Test_UpdateChecker::parsesTags() {
    QFETCH(QString, tag);
    QFETCH(QVersionNumber, expected);
    QCOMPARE(UpdateChecker::versionFromTag(tag), expected);
}

void Test_UpdateChecker::comparesVersions_data() {
    QTest::addColumn<QString>("tag");
    QTest::addColumn<QVersionNumber>("current");
    QTest::addColumn<bool>("isNewer");

    QTest::newRow("same") << "v2.0.0" << QVersionNumber(2, 0, 0) << false;
    QTest::newRow("older") << "v1.9.9" << QVersionNumber(2, 0, 0) << false;
    QTest::newRow("patch bump") << "v2.0.1" << QVersionNumber(2, 0, 0) << true;
    QTest::newRow("minor bump") << "v2.1.0" << QVersionNumber(2, 0, 0) << true;
    QTest::newRow("major bump") << "v3.0.0" << QVersionNumber(2, 0, 0) << true;
    // String comparison would get this backwards: "2.0.9" > "2.0.10".
    QTest::newRow("double digits") << "v2.0.10" << QVersionNumber(2, 0, 9) << true;
    QTest::newRow("not a downgrade") << "v2.0.9" << QVersionNumber(2, 0, 10) << false;

    QTest::newRow("rc is not newer than its release") << "v2.0.0-rc1" << QVersionNumber(2, 0, 0) << false;
}

void Test_UpdateChecker::comparesVersions() {
    QFETCH(QString, tag);
    QFETCH(QVersionNumber, current);
    QFETCH(bool, isNewer);
    QCOMPARE(UpdateChecker::versionFromTag(tag) > current, isNewer);
}

// A fork that still points its update check at the upstream repository would
// tell every user to "upgrade" to a completely different application.
void Test_UpdateChecker::releasesUrlPointsAtThisFork() {
    QVERIFY2(UpdateChecker::releasesUrl().contains(QStringLiteral("duckautomata/qimgv")),
             qPrintable(UpdateChecker::releasesUrl()));
}

QTEST_GUILESS_MAIN(Test_UpdateChecker)
#include "test_updatechecker.moc"
