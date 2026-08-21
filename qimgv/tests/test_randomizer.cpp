// Regression tests for Randomizer.
//
// The original implementation used `while(currentIndex == vec.size() - 1)`.
// vec.size() is unsigned, so on an empty vector that condition compares
// against SIZE_MAX and the body read vec[-1]; on a single-element vector the
// item is always simultaneously first and last, so the loop never terminated
// and shuffle mode hung the app on any directory containing exactly one image.
#include <QtTest>
#include "utils/randomizer.h"

class Test_Randomizer : public QObject {
    Q_OBJECT

private slots:
    void emptyDoesNotHang();
    void singleElementDoesNotHang();
    void staysInRange_data();
    void staysInRange();
    void visitsEveryElement();
};

void Test_Randomizer::emptyDoesNotHang() {
    Randomizer r;
    r.setCount(0);
    QCOMPARE(r.next(), -1);
    QCOMPARE(r.prev(), -1);
}

void Test_Randomizer::singleElementDoesNotHang() {
    Randomizer r;
    r.setCount(1);
    r.setCurrent(0);
    // Both directions must terminate and yield the only element.
    QCOMPARE(r.next(), 0);
    QCOMPARE(r.prev(), 0);
    QCOMPARE(r.next(), 0);
}

void Test_Randomizer::staysInRange_data() {
    QTest::addColumn<int>("count");
    for(int n : {1, 2, 3, 5, 16, 100})
        QTest::addRow("count=%d", n) << n;
}

// Walking far past the end must keep returning valid indices, never -1 or
// an out-of-range value, and must always terminate.
void Test_Randomizer::staysInRange() {
    QFETCH(int, count);
    Randomizer r;
    r.setCount(count);
    r.setCurrent(0);

    for(int i = 0; i < count * 4 + 10; i++) {
        const int v = r.next();
        QVERIFY2(v >= 0 && v < count, qPrintable(QStringLiteral("next() = %1").arg(v)));
    }
    for(int i = 0; i < count * 4 + 10; i++) {
        const int v = r.prev();
        QVERIFY2(v >= 0 && v < count, qPrintable(QStringLiteral("prev() = %1").arg(v)));
    }
}

// One full pass must be a permutation: every index exactly once.
void Test_Randomizer::visitsEveryElement() {
    constexpr int kCount = 20;
    Randomizer r;
    r.setCount(kCount);
    r.setCurrent(0);

    QSet<int> seen{0};
    for(int i = 0; i < kCount - 1; i++)
        seen.insert(r.next());

    QCOMPARE(seen.size(), kCount);
}

QTEST_APPLESS_MAIN(Test_Randomizer)
#include "test_randomizer.moc"
