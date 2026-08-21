// indexOfFile() and indexOfDir() are backed by a path -> index hash that is
// rebuilt lazily. That is only correct if every mutation invalidates it, and a
// missed invalidation does not crash or warn -- it silently returns an index
// belonging to some other file, which downstream code happily uses to show the
// wrong image or overwrite the wrong entry.
//
// So these tests care less about the lookups themselves than about every path
// that reorders or resizes the lists underneath them. Each one mutates, then
// checks the whole mapping still agrees with filePathAt().
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QImage>
#include <QStandardPaths>

#include "components/directorymanager/directorymanager.h"
#include "settings.h"

class Test_DirectoryManager : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void indexMatchesOrderAfterScan();
    void indexSurvivesInsert();
    void indexSurvivesRemove();
    void indexSurvivesRename();
    void indexSurvivesResort();
    void missingPathIsNotFound();
    void navigationAgreesWithOrder();

private:
    QTemporaryDir dir;
    void writeImage(QString const &name) {
        QImage img(8, 8, QImage::Format_RGB32);
        img.fill(Qt::blue);
        QVERIFY(img.save(dir.filePath(name), "png"));
    }
    // The property that matters: for every index, indexOfFile(pathAt(i)) == i.
    void verifyMappingIsConsistent(DirectoryManager &dm) {
        for(int i = 0; i < static_cast<int>(dm.fileCount()); i++) {
            QString const path = dm.filePathAt(i);
            QCOMPARE(dm.indexOfFile(path), i);
        }
    }
};

void Test_DirectoryManager::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("qimgv-test");
    QCoreApplication::setApplicationName("qimgv-test");
    settings = Settings::getInstance();
    QVERIFY(dir.isValid());
    for(int i = 0; i < 12; i++)
        writeImage(QStringLiteral("img%1.png").arg(i, 2, 10, QLatin1Char('0')));
}

void Test_DirectoryManager::indexMatchesOrderAfterScan() {
    DirectoryManager dm;
    QVERIFY(dm.setDirectory(dir.path()));
    QCOMPARE(static_cast<int>(dm.fileCount()), 12);
    verifyMappingIsConsistent(dm);
}

void Test_DirectoryManager::indexSurvivesInsert() {
    DirectoryManager dm;
    QVERIFY(dm.setDirectory(dir.path()));
    // Warm the cache first: inserting without a prior lookup would pass even
    // with the invalidation missing.
    QCOMPARE(dm.indexOfFile(dm.filePathAt(0)), 0);

    // Sorts to the front, so every existing index shifts by one.
    writeImage(QStringLiteral("aaa_inserted.png"));
    QVERIFY(dm.insertFileEntry(dir.filePath(QStringLiteral("aaa_inserted.png"))));
    QCOMPARE(static_cast<int>(dm.fileCount()), 13);
    QCOMPARE(dm.indexOfFile(dir.filePath(QStringLiteral("aaa_inserted.png"))), 0);
    verifyMappingIsConsistent(dm);
}

void Test_DirectoryManager::indexSurvivesRemove() {
    DirectoryManager dm;
    QVERIFY(dm.setDirectory(dir.path()));
    int const before = static_cast<int>(dm.fileCount());
    QString const doomed = dm.filePathAt(0);
    QCOMPARE(dm.indexOfFile(doomed), 0); // warm

    dm.removeFileEntry(doomed);
    QCOMPARE(static_cast<int>(dm.fileCount()), before - 1);
    QCOMPARE(dm.indexOfFile(doomed), -1);
    verifyMappingIsConsistent(dm);
}

void Test_DirectoryManager::indexSurvivesRename() {
    DirectoryManager dm;
    QVERIFY(dm.setDirectory(dir.path()));
    QString const oldPath = dm.filePathAt(1);
    QCOMPARE(dm.indexOfFile(oldPath), 1); // warm

    // renameFileEntry() reads the new path off disk, so the file has to move
    // first -- that is the order the application does it in, and skipping it
    // makes the entry look unsupported and simply disappear.
    QVERIFY(QFile::rename(oldPath, dir.filePath(QStringLiteral("zzz_renamed.png"))));

    // Renaming moves the entry to the end of the ordering as well as changing
    // its key, so both the old and the new path have to be right afterwards.
    dm.renameFileEntry(oldPath, QStringLiteral("zzz_renamed.png"));
    QCOMPARE(dm.indexOfFile(oldPath), -1);
    int const renamed = dm.indexOfFile(dir.filePath(QStringLiteral("zzz_renamed.png")));
    QVERIFY2(renamed != -1, "renamed file is not findable by its new path");
    QCOMPARE(dm.filePathAt(renamed), dir.filePath(QStringLiteral("zzz_renamed.png")));
    verifyMappingIsConsistent(dm);
}

void Test_DirectoryManager::indexSurvivesResort() {
    DirectoryManager dm;
    QVERIFY(dm.setDirectory(dir.path()));
    QString const first = dm.filePathAt(0);
    QCOMPARE(dm.indexOfFile(first), 0); // warm

    // Reversing the order keeps every path but changes every index, which is
    // exactly what a stale cache cannot survive.
    dm.setSortingMode(SORT_NAME_DESC);
    verifyMappingIsConsistent(dm);
    QCOMPARE(dm.filePathAt(static_cast<int>(dm.fileCount()) - 1), first);

    dm.setSortingMode(SORT_NAME);
    verifyMappingIsConsistent(dm);
    QCOMPARE(dm.filePathAt(0), first);
}

void Test_DirectoryManager::missingPathIsNotFound() {
    DirectoryManager dm;
    QVERIFY(dm.setDirectory(dir.path()));
    QCOMPARE(dm.indexOfFile(dir.filePath(QStringLiteral("nope.png"))), -1);
    QCOMPARE(dm.indexOfFile(QString()), -1);
    QCOMPARE(dm.indexOfDir(dir.filePath(QStringLiteral("nope"))), -1);
}

// prevOfFile/nextOfFile are built on indexOfFile, so a broken cache shows up
// here as navigation that skips or repeats files.
void Test_DirectoryManager::navigationAgreesWithOrder() {
    DirectoryManager dm;
    QVERIFY(dm.setDirectory(dir.path()));
    QCOMPARE(dm.prevOfFile(dm.firstFile()), QString());
    QCOMPARE(dm.nextOfFile(dm.lastFile()), QString());

    QString cur = dm.firstFile();
    for(int i = 1; i < static_cast<int>(dm.fileCount()); i++) {
        cur = dm.nextOfFile(cur);
        QCOMPARE(cur, dm.filePathAt(i));
    }
    for(int i = static_cast<int>(dm.fileCount()) - 2; i >= 0; i--) {
        cur = dm.prevOfFile(cur);
        QCOMPARE(cur, dm.filePathAt(i));
    }
}

QTEST_MAIN(Test_DirectoryManager)
#include "test_directorymanager.moc"
