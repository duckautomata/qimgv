// Media and volume keys: the names ShortcutBuilder gives them on every platform, and how ActionManager
// decides whether qimgv takes one or passes it on to the system and other players. Ordinary keys are
// checked alongside, to show the media key path leaves them as they were.
#include <QtTest>
#include "components/actionmanager/actionmanager.h"
#include "components/scriptmanager/scriptmanager.h"
#include "settings.h"
#include "shortcutbuilder.h"
#include "utils/actions.h"
#include "utils/inputmap.h"

namespace {
QKeyEvent keyPress(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, quint32 scanCode = 0,
                   QString const &text = QString()) {
    return QKeyEvent(QEvent::KeyPress, key, modifiers, scanCode, 0, 0, text);
}

// The scan code InputMap reads as `name`; 0 where there are none (macOS names every key from its text).
quint32 scanCodeOf(QString const &name) {
    auto const &keys = inputMap->keys();
    for(auto i = keys.cbegin(); i != keys.cend(); ++i) {
        if(i.value() == name)
            return i.key();
    }
    return 0;
}
} // namespace

class Test_ShortcutBuilder : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void mediaKeyNames_data();
    void mediaKeyNames();
    void mediaKeyWithModifiers();
    void mediaKeyIgnoresScanCode();
    void mediaKeyReleaseHasNoName();
    void ordinaryKeysUnchanged();
    void defaultsLeaveVolumeKeysAlone();
    void mediaKeyTakenWhenFilterAllows();
    void mediaKeyPassedOnWhenFilterRefuses();
    void unboundMediaKeyPassedOn();
    void ordinaryKeysIgnoreFilter();
    void updateAssignsFreeMediaKeysOnly();

private:
    // The bindings each test starts from: the defaults, as a fresh install has them.
    void resetShortcuts();
};

void Test_ShortcutBuilder::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("qimgv-test");
    QCoreApplication::setApplicationName("qimgv-shortcut-test");
    // In main.cpp's order: the action manager reads the others.
    inputMap = InputMap::getInstance();
    appActions = Actions::getInstance();
    settings = Settings::getInstance();
    scriptManager = ScriptManager::getInstance();
    actionManager = ActionManager::getInstance();
}

void Test_ShortcutBuilder::init() {
    resetShortcuts();
    actionManager->setMediaKeyFilter(nullptr);
}

void Test_ShortcutBuilder::resetShortcuts() {
    actionManager->resetDefaults();
}

void Test_ShortcutBuilder::mediaKeyNames_data() {
    QTest::addColumn<int>("key");
    QTest::addColumn<QString>("name");
    // The names macOS has always produced for these keys, so bindings carry across platforms.
    QTest::newRow("play/pause") << int(Qt::Key_MediaTogglePlayPause) << "MediaTogglePlayPause";
    QTest::newRow("play") << int(Qt::Key_MediaPlay) << "MediaPlay";
    QTest::newRow("pause") << int(Qt::Key_MediaPause) << "MediaPause";
    QTest::newRow("stop") << int(Qt::Key_MediaStop) << "MediaStop";
    QTest::newRow("next") << int(Qt::Key_MediaNext) << "MediaNext";
    QTest::newRow("previous") << int(Qt::Key_MediaPrevious) << "MediaPrevious";
    QTest::newRow("volume up") << int(Qt::Key_VolumeUp) << "VolumeUp";
    QTest::newRow("volume down") << int(Qt::Key_VolumeDown) << "VolumeDown";
    QTest::newRow("mute") << int(Qt::Key_VolumeMute) << "VolumeMute";
}

void Test_ShortcutBuilder::mediaKeyNames() {
    QFETCH(int, key);
    QFETCH(QString, name);
    QVERIFY(ShortcutBuilder::isMediaKey(Qt::Key(key)));
    auto event = keyPress(key);
    QCOMPARE(ShortcutBuilder::fromEvent(&event), name);
}

void Test_ShortcutBuilder::mediaKeyWithModifiers() {
    auto ctrl = keyPress(Qt::Key_MediaNext, Qt::ControlModifier);
    QCOMPARE(ShortcutBuilder::fromEvent(&ctrl), InputMap::keyNameCtrl() + "+MediaNext");
    auto shift = keyPress(Qt::Key_VolumeUp, Qt::ShiftModifier);
    QCOMPARE(ShortcutBuilder::fromEvent(&shift), InputMap::keyNameShift() + "+VolumeUp");
    // Several, in the order every other shortcut lists them.
    QStringList names = {InputMap::keyNameCtrl(), InputMap::keyNameShift()};
    std::sort(names.begin(), names.end());
    auto both = keyPress(Qt::Key_MediaTogglePlayPause, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(ShortcutBuilder::fromEvent(&both), names.join("+") + "+MediaTogglePlayPause");
}

void Test_ShortcutBuilder::mediaKeyIgnoresScanCode() {
    // Windows reports media keys with the scan code of an ordinary key, less the extended-key bit: next
    // track shares one with P. Read through the scan code table, it would be taken for P.
    quint32 const p = scanCodeOf("P");
    auto event = keyPress(Qt::Key_MediaNext, Qt::NoModifier, p);
    QCOMPARE(ShortcutBuilder::fromEvent(&event), QString("MediaNext"));
}

void Test_ShortcutBuilder::mediaKeyReleaseHasNoName() {
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_MediaNext, Qt::NoModifier);
    QVERIFY(ShortcutBuilder::fromEvent(&release).isEmpty());
}

void Test_ShortcutBuilder::ordinaryKeysUnchanged() {
    QVERIFY(!ShortcutBuilder::isMediaKey(Qt::Key_A));
    QVERIFY(!ShortcutBuilder::isMediaKey(Qt::Key_Space));
    auto a = keyPress(Qt::Key_A, Qt::NoModifier, scanCodeOf("A"), "a");
    QCOMPARE(ShortcutBuilder::fromEvent(&a), QString("A"));
    auto ctrlA = keyPress(Qt::Key_A, Qt::ControlModifier, scanCodeOf("A"), "\x01");
    QCOMPARE(ShortcutBuilder::fromEvent(&ctrlA), InputMap::keyNameCtrl() + "+A");
    // Modifiers alone name nothing.
    auto ctrl = keyPress(Qt::Key_Control, Qt::ControlModifier);
    QVERIFY(ShortcutBuilder::fromEvent(&ctrl).isEmpty());
}

void Test_ShortcutBuilder::defaultsLeaveVolumeKeysAlone() {
    auto const &shortcuts = actionManager->allShortcuts();
    QCOMPARE(shortcuts.value("MediaTogglePlayPause"), QString("pauseVideo"));
    QCOMPARE(shortcuts.value("MediaPlay"), QString("pauseVideo"));
    QCOMPARE(shortcuts.value("MediaPause"), QString("pauseVideo"));
    QCOMPARE(shortcuts.value("MediaNext"), QString("nextImage"));
    QCOMPARE(shortcuts.value("MediaPrevious"), QString("prevImage"));
    // The system's volume, not ours.
    QVERIFY(!shortcuts.contains("VolumeUp"));
    QVERIFY(!shortcuts.contains("VolumeDown"));
    QVERIFY(!shortcuts.contains("VolumeMute"));
}

void Test_ShortcutBuilder::mediaKeyTakenWhenFilterAllows() {
    QList<Qt::Key> asked;
    actionManager->setMediaKeyFilter([&asked](Qt::Key key) {
        asked.append(key);
        return true;
    });
    QSignalSpy next(actionManager, &ActionManager::nextImage);
    auto event = keyPress(Qt::Key_MediaNext);
    QVERIFY(actionManager->processEvent(&event));
    QVERIFY(event.isAccepted());
    QCOMPARE(next.count(), 1);
    QCOMPARE(asked.count(), 1);
    QCOMPARE(asked.first(), Qt::Key_MediaNext);
}

void Test_ShortcutBuilder::mediaKeyPassedOnWhenFilterRefuses() {
    actionManager->setMediaKeyFilter([](Qt::Key) { return false; });
    QSignalSpy pause(actionManager, &ActionManager::pauseVideo);
    QSignalSpy next(actionManager, &ActionManager::nextImage);
    auto playPause = keyPress(Qt::Key_MediaTogglePlayPause);
    // Accepted on the way in, as every widget's key handler does before passing the event on.
    playPause.accept();
    QVERIFY(!actionManager->processEvent(&playPause));
    // Ignored, so the window hands it back to the system.
    QVERIFY(!playPause.isAccepted());
    auto nextKey = keyPress(Qt::Key_MediaNext);
    QVERIFY(!actionManager->processEvent(&nextKey));
    QVERIFY(!nextKey.isAccepted());
    QCOMPARE(pause.count(), 0);
    QCOMPARE(next.count(), 0);
}

void Test_ShortcutBuilder::unboundMediaKeyPassedOn() {
    actionManager->setMediaKeyFilter([](Qt::Key) { return true; });
    QSignalSpy volume(actionManager, &ActionManager::volumeUp);
    auto event = keyPress(Qt::Key_VolumeUp);
    event.accept();
    QVERIFY(!actionManager->processEvent(&event));
    QVERIFY(!event.isAccepted());
    QCOMPARE(volume.count(), 0);
    // Bound by the user, it works as any other binding.
    actionManager->addShortcut("VolumeUp", "volumeUp");
    auto bound = keyPress(Qt::Key_VolumeUp);
    QVERIFY(actionManager->processEvent(&bound));
    QVERIFY(bound.isAccepted());
    QCOMPARE(volume.count(), 1);
}

void Test_ShortcutBuilder::ordinaryKeysIgnoreFilter() {
    int asked = 0;
    actionManager->setMediaKeyFilter([&asked](Qt::Key) {
        asked++;
        return false;
    });
    QSignalSpy next(actionManager, &ActionManager::nextImage);
    auto right = keyPress(Qt::Key_Right, Qt::NoModifier, scanCodeOf("Right"));
    QVERIFY(actionManager->processEvent(&right));
    QVERIFY(right.isAccepted());
    QCOMPARE(next.count(), 1);
    QCOMPARE(asked, 0);
}

void Test_ShortcutBuilder::updateAssignsFreeMediaKeysOnly() {
    // A 2.0 user: no media keys bound by default then, but one could have been bound by hand -- on macOS
    // they always could -- and A was free.
    actionManager->removeAllShortcuts();
    actionManager->addShortcut("Right", "nextImage");
    actionManager->addShortcut("MediaNext", "toggleFullscreen");
    actionManager->adjustFromVersion(QVersionNumber(2, 0, 3));
    auto const &shortcuts = actionManager->allShortcuts();
    QCOMPARE(shortcuts.value("MediaNext"), QString("toggleFullscreen"));
    QCOMPARE(shortcuts.value("MediaPrevious"), QString("prevImage"));
    QCOMPARE(shortcuts.value("MediaTogglePlayPause"), QString("pauseVideo"));
    QCOMPARE(shortcuts.value("A"), QString("cycleAudioMode"));
    QVERIFY(!shortcuts.contains("VolumeUp"));
    QVERIFY(!shortcuts.contains("VolumeDown"));
    QVERIFY(!shortcuts.contains("VolumeMute"));
}

QTEST_MAIN(Test_ShortcutBuilder)
#include "test_shortcutbuilder.moc"
