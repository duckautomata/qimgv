// Exercises DocumentInfo's content sniffing against hand-built fixtures that
// target the exact edge cases the old detectors got wrong:
//   - APNG whose acTL chunk sits past the first 120 bytes
//   - AVIF sequences that declare 'avis' only in the compatible-brands list
#include <QtTest>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QImageReader>
#include <QMovie>

#include "settings.h"
#include "sourcecontainers/documentinfo.h"

class Test_FormatDetect : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void detectsType_data();
    void detectsType();
    void decodesAnimatedAvif();

private:
    QString dataPath(const QString &name) const {
        return QStringLiteral(QIMGV_TEST_DATA_DIR) + QLatin1Char('/') + name;
    }
};

void Test_FormatDetect::initTestCase() {
    // Keep the test out of the developer's real config directory.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("qimgv-test");
    QCoreApplication::setApplicationName("qimgv-test");
    settings = Settings::getInstance();
    QVERIFY(settings != nullptr);
}

void Test_FormatDetect::detectsType_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<int>("expectedType");
    QTest::addColumn<QByteArray>("requiresReader");

    // The regression that motivated rewriting detectAPNG(): 200 bytes of text
    // metadata ahead of acTL. The old 120-byte window missed it.
    // qimgv only reports APNG as animated when an "apng" reader is present,
    // so these rows are skipped on a system without that plugin.
    QTest::newRow("apng, acTL past 120 bytes") << "apng_late_actl.png" << int(ANIMATED) << QByteArray("apng");
    QTest::newRow("apng, acTL early") << "apng_early_actl.png" << int(ANIMATED) << QByteArray("apng");
    QTest::newRow("png, no acTL") << "png_still.png" << int(STATIC) << QByteArray();

    // The regression that motivated rewriting detectAnimatedAvif(): major
    // brand is 'avif', 'avis' appears only among the compatible brands.
    QTest::newRow("avif seq via compat brand") << "avif_seq_compat.avif" << int(ANIMATED) << QByteArray();
    QTest::newRow("avif seq via major brand") << "avif_seq_major.avif" << int(ANIMATED) << QByteArray();
    QTest::newRow("avif still") << "avif_still.avif" << int(STATIC) << QByteArray();

    QTest::newRow("webp animated") << "webp_animated.webp" << int(ANIMATED) << QByteArray();
    QTest::newRow("webp still") << "webp_still.webp" << int(STATIC) << QByteArray();

    // Real files produced by ffmpeg/libaom, not hand-built headers.
    QTest::newRow("real avif sequence") << "real_anim.avif" << int(ANIMATED) << QByteArray();
    QTest::newRow("real avif still") << "real_still.avif" << int(STATIC) << QByteArray();
}

void Test_FormatDetect::detectsType() {
    QFETCH(QString, file);
    QFETCH(int, expectedType);
    QFETCH(QByteArray, requiresReader);

    if(!requiresReader.isEmpty() && !QImageReader::supportedImageFormats().contains(requiresReader)) {
        QSKIP(
            qPrintable(QStringLiteral("no '%1' image plugin on this system").arg(QString::fromLatin1(requiresReader))));
    }

    const QString path = dataPath(file);
    QVERIFY2(QFile::exists(path), qPrintable(path));

    DocumentInfo info(path);
    QCOMPARE(int(info.type()), expectedType);
}

// Detection says "this file is animated"; this checks that the installed
// plugin can actually decode it, which is what decides whether the user sees
// an animation or one frozen frame.
void Test_FormatDetect::decodesAnimatedAvif() {
    if(!QImageReader::supportedImageFormats().contains(QByteArrayLiteral("avif")))
        QSKIP("no 'avif' image plugin on this system (install kimageformats)");

    const QString path = dataPath(QStringLiteral("real_anim.avif"));
    QVERIFY2(QFile::exists(path), qPrintable(path));

    QImageReader reader(path);
    QVERIFY2(reader.canRead(), qPrintable(reader.errorString()));
    QCOMPARE(reader.format(), QByteArray("avif"));
    QVERIFY(reader.supportsAnimation());
    QCOMPARE(reader.imageCount(), 10);

    // qimgv drives ANIMATED documents through QMovie, so cover that path too.
    QMovie movie(path);
    movie.setFormat("avif");
    QVERIFY(movie.isValid());
    QCOMPARE(movie.frameCount(), 10);

    for(int i = 0; i < 10; i++) {
        QVERIFY(movie.jumpToFrame(i));
        QVERIFY2(!movie.currentImage().isNull(), qPrintable(QStringLiteral("frame %1 decoded to a null image").arg(i)));
    }
}

QTEST_MAIN(Test_FormatDetect)
#include "test_formatdetect.moc"
