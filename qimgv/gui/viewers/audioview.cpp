#include "audioview.h"

#include "audioplayer.h"
#include "playerplugin.h"
#include "components/actionmanager/actionmanager.h"
#include "components/audiometa/audiometadata.h"
#include "gui/customwidgets/actionbutton.h"
#include "gui/customwidgets/mediaslider.h"
#include "utils/imagelib.h"
#include <QAccessibleWidget>
#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QThreadPool>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Layout, in device-independent pixels.
constexpr int kMargin = 16;
// FloatingMessage sits 35 px above the bottom edge. Controls stay clear of it when there is room.
constexpr int kMessageReserve = 60;
// MainPanel's thickness beyond the thumbnail size: its frame, padding and scroll bar.
constexpr int kPanelFrame = 40;
constexpr int kCoverGap = 24;
constexpr int kCoverMin = 96;
constexpr int kCoverMax = 560;
constexpr qreal kCoverRadius = 8;
// Room around the cover for its shadow.
constexpr int kShadowPad = 32;
constexpr int kCardMinWidth = 272;
// Enough for the volume slider. The card is never narrower than this while the window has room, even
// below a smaller cover; a wide layout shrinks the cover before the card.
constexpr int kCardWidth = 420;
constexpr int kCardMaxWidth = 560;
constexpr int kPadH = 20;
constexpr int kPadV = 16;
constexpr int kLineGap = 3;
constexpr int kTextToSeek = 12;
constexpr int kSeekHeight = 20;
constexpr int kSeekToTransport = 4;
constexpr int kTimeGap = 4;
constexpr int kButton = 36;
constexpr int kPlayButton = 48;
constexpr int kTransportGap = 8;
constexpr int kVolumeGap = 12;
constexpr int kVolumeMin = 56;
constexpr int kVolumeMax = 96;
// Windows shorter than this and wider than kWideAspect times their height put the cover on the left.
constexpr int kWideMaxHeight = 420;
constexpr qreal kWideAspect = 1.3;

constexpr double kSeekWheelStep = 5;
constexpr int kSeekDragInterval = 80;
constexpr int kVolumeStep = 5;
// How long the previous track's title and cover stay up while the next one's tags are read. Without it,
// skipping through an album flashes the file name and an empty tile between every two tracks.
constexpr int kMetadataGrace = 250;
constexpr qreal kBackdropOpacity = 0.3;

const QString kButtonIcons = QStringLiteral(":res/icons/common/buttons/audio/");
const QString kPlaceholderIcon = QStringLiteral(":res/icons/common/other/audio-file72.png");

QFont scaledFont(QFont font, qreal factor, QFont::Weight weight = QFont::Normal) {
    if(font.pointSizeF() > 0)
        font.setPointSizeF(std::max(7.0, font.pointSizeF() * factor));
    else if(font.pixelSize() > 0)
        font.setPixelSize(std::max(9, qRound(font.pixelSize() * factor)));
    font.setWeight(weight);
    return font;
}

QColor mix(QColor const &a, QColor const &b, qreal t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

qreal luminance(QColor const &c) {
    auto channel = [](qreal v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

qreal contrast(QColor const &a, QColor const &b) {
    qreal la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// m:ss, or h:mm:ss for anything an hour or longer, so that elapsed and total time line up.
QString formatTime(double seconds, bool hours) {
    qint64 s = seconds > 0 ? static_cast<qint64>(seconds) : 0;
    if(hours)
        return QStringLiteral("%1:%2:%3")
            .arg(s / 3600)
            .arg(s / 60 % 60, 2, 10, QChar('0'))
            .arg(s % 60, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

// The widest text formatTime() produces for a track of `duration`. The hours are not padded, and an audiobook
// can run to ten or more of them.
QString timePattern(double duration) {
    if(duration >= 3600)
        return QString(QString::number(static_cast<qint64>(duration) / 3600).size(), QChar('0')) +
               QStringLiteral(":00:00");
    return duration >= 600 ? QStringLiteral("00:00") : QStringLiteral("0:00");
}

// mpv reports libavcodec's short names. Most read better under their usual name.
QString codecName(QString const &codec) {
    static const QHash<QString, QString> names = {
        {"aac", "AAC"},
        {"aac_latm", "AAC"},
        {"ac3", "AC-3"},
        {"alac", "ALAC"},
        {"amrnb", "AMR-NB"},
        {"amrwb", "AMR-WB"},
        {"ape", "Monkey's Audio"},
        {"atrac3", "ATRAC3"},
        {"atrac3p", "ATRAC3+"},
        {"cook", "RealAudio"},
        {"dts", "DTS"},
        {"eac3", "E-AC-3"},
        {"flac", "FLAC"},
        {"gsm", "GSM"},
        {"mlp", "MLP"},
        {"mp1", "MP1"},
        {"mp2", "MP2"},
        {"mp3", "MP3"},
        {"musepack7", "Musepack"},
        {"musepack8", "Musepack"},
        {"opus", "Opus"},
        {"pcm_alaw", "A-law"},
        {"pcm_mulaw", "μ-law"},
        {"shorten", "Shorten"},
        {"speex", "Speex"},
        {"tak", "TAK"},
        {"truehd", "TrueHD"},
        {"tta", "TTA"},
        {"vorbis", "Vorbis"},
        {"wavpack", "WavPack"},
        {"wmalossless", "WMA Lossless"},
        {"wmapro", "WMA Pro"},
        {"wmav1", "WMA"},
        {"wmav2", "WMA"},
        {"wmavoice", "WMA Voice"},
    };
    QString key = codec.toLower();
    auto it = names.constFind(key);
    if(it != names.constEnd())
        return it.value();
    if(key.startsWith("pcm_"))
        return QStringLiteral("PCM");
    if(key.startsWith("adpcm_"))
        return QStringLiteral("ADPCM");
    if(key.startsWith("dsd_"))
        return QStringLiteral("DSD");
    return codec.toUpper();
}

// Bits per sample, where the stream's own figure is known: 0 otherwise.
int bitDepth(QString const &codec, QString const &sampleFormat) {
    static const QRegularExpression pcm(QStringLiteral("^pcm_[suf](\\d+)"));
    QString key = codec.toLower();
    auto match = pcm.match(key);
    if(match.hasMatch())
        return match.captured(1).toInt();
    static const QStringList lossless = {"flac", "alac", "wavpack", "ape",         "tta",
                                         "tak",  "mlp",  "truehd",  "wmalossless", "shorten"};
    if(!lossless.contains(key))
        return 0;
    // The decoder's output format. 16 bits means the source had at most 16; 32 holds anything from 17 to 32,
    // and a guess would be wrong as often as not.
    if(sampleFormat.startsWith("u8"))
        return 8;
    if(sampleFormat.startsWith("s16"))
        return 16;
    return 0;
}

QString channelsName(int channels, QString const &layout) {
    if(layout == "mono" || (layout.isEmpty() && channels == 1))
        return AudioView::tr("Mono");
    if(layout == "stereo" || (layout.isEmpty() && channels == 2))
        return AudioView::tr("Stereo");
    // mpv names surround layouts "5.1(side)", "7.1(wide)" and so on; the qualifier means little here.
    static const QRegularExpression surround(QStringLiteral("^\\d+\\.\\d+"));
    auto match = surround.match(layout);
    if(match.hasMatch())
        return match.captured(0);
    if(channels > 0)
        return AudioView::tr("%n channels", "", channels);
    return {};
}

// "FLAC · 44.1 kHz · 16-bit · Stereo · 912 kbps", leaving out whatever is not known.
QString formatLine(QVariantMap const &format) {
    QStringList parts;
    QString codec = format.value("codec").toString();
    if(!codec.isEmpty())
        parts << codecName(codec);
    int sampleRate = format.value("samplerate").toInt();
    if(sampleRate > 0)
        parts << AudioView::tr("%1 kHz").arg(QString::number(sampleRate / 1000.0, 'g', 6));
    int bits = bitDepth(codec, format.value("sample-format").toString());
    if(bits > 0)
        parts << AudioView::tr("%1-bit").arg(bits);
    QString channels = channelsName(format.value("channels").toInt(), format.value("channel-layout").toString());
    if(!channels.isEmpty())
        parts << channels;
    // Rounded first: a near-silent stream measures a few hundred bits per second, and "0 kbps" says nothing.
    const qint64 kbps = qRound64(format.value("bitrate").toLongLong() / 1000.0);
    if(kbps > 0)
        parts << AudioView::tr("%1 kbps").arg(kbps);
    return parts.join(QStringLiteral(" · "));
}

QString modeName(AudioPlaybackMode mode) {
    switch(mode) {
    case AUDIO_MODE_SINGLE:
        break;
    case AUDIO_MODE_REPEAT:
        return AudioView::tr("Repeat track");
    case AUDIO_MODE_FOLDER:
        return AudioView::tr("Play folder");
    case AUDIO_MODE_SHUFFLE:
        return AudioView::tr("Shuffle");
    }
    return AudioView::tr("Single file");
}

QString modeIcon(AudioPlaybackMode mode) {
    switch(mode) {
    case AUDIO_MODE_SINGLE:
        break;
    case AUDIO_MODE_REPEAT:
        return kButtonIcons + "mode-repeat24.png";
    case AUDIO_MODE_FOLDER:
        return kButtonIcons + "mode-folder24.png";
    case AUDIO_MODE_SHUFFLE:
        return kButtonIcons + "mode-shuffle24.png";
    }
    return kButtonIcons + "mode-single24.png";
}

// The binding a tooltip names for `action`. A key on the keyboard if there is one: a media key, a mouse button or
// the wheel says little to someone looking at a button, and ActionManager lists the bindings alphabetically.
// Among equals, the shortest, which has the fewest modifiers.
QString shortcutFor(QString const &action) {
    if(!actionManager)
        return {};
    static const QRegularExpression notKeyboard(
        QStringLiteral("(^|\\+)(Media|Volume|Wheel|LMB|RMB|MiddleButton|XButton)"));
    QString best;
    int bestRank = 0;
    for(QString const &keys : actionManager->shortcutsForAction(action)) {
        int rank = (notKeyboard.match(keys).hasMatch() ? 1000 : 0) + static_cast<int>(keys.size());
        if(best.isEmpty() || rank < bestRank) {
            best = keys;
            bestRank = rank;
        }
    }
    return best;
}

QString withShortcut(QString const &text, QString const &action) {
    QString keys = shortcutFor(action);
    return keys.isEmpty() ? text : QStringLiteral("%1 (%2)").arg(text, keys);
}

// `color` made lighter or darker, keeping its hue, until it stands out from `ground` by `ratio`: an accent close
// to the track would leave the played part of the seek bar hard to tell from the rest.
QColor legibleOn(QColor const &color, QColor const &ground, qreal ratio) {
    if(contrast(color, ground) >= ratio)
        return color;
    float h, s, l, a;
    color.getHslF(&h, &s, &l, &a);
    // Away from the ground first: lighter on a dark track, darker on a light one.
    const float away = luminance(ground) < 0.18 ? 1.0f : -1.0f;
    QColor best = color;
    for(float direction : {away, -away}) {
        for(int i = 1; i <= 50; ++i) {
            float lightness = l + direction * 0.02f * i;
            if(lightness < 0 || lightness > 1)
                break;
            QColor candidate = QColor::fromHslF(h, s, lightness, a);
            if(contrast(candidate, ground) >= ratio)
                return candidate;
            if(contrast(candidate, ground) > contrast(best, ground))
                best = candidate;
        }
    }
    return best;
}

// A transport button. The left button is all it uses: anything else goes on to the viewer as it does over the
// rest of the view (the context menu, the thumb buttons' previous and next), and so do hover moves, which the
// panel's trigger and the click-zone highlight run on.
class TransportButton : public ActionButton {
public:
    TransportButton(QString const &action, QString const &icon, int size, QWidget *parent)
        : ActionButton(action, icon, size, parent) {
        setMouseTracking(true);
    }

    void trigger() {
        if(isEnabled())
            actionManager->invokeAction(actionName);
    }

protected:
    // A double click reaches mousePressEvent through QWidget's handler, so a quick second click on Next is
    // another Next, not fullscreen.
    void mousePressEvent(QMouseEvent *event) override {
        if(event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }
        ActionButton::mousePressEvent(event);
    }

    // The release follows its press here even when that press was passed on.
    void mouseReleaseEvent(QMouseEvent *event) override {
        if(event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }
        ActionButton::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        ActionButton::mouseMoveEvent(event);
        // Only a press made here makes the moves its own.
        if(!mPressed)
            event->ignore();
    }
};

#if QT_CONFIG(accessibility)
// A button to assistive technology, which can press it, rather than a client area.
class TransportButtonAccessible : public QAccessibleWidget {
public:
    explicit TransportButtonAccessible(TransportButton *button) : QAccessibleWidget(button, QAccessible::Button) {}

    QStringList actionNames() const override { return {pressAction()}; }

    void doAction(QString const &name) override {
        if(name == pressAction())
            static_cast<TransportButton *>(widget())->trigger();
    }
};

QAccessibleInterface *createAccessible(QString const & /*key*/, QObject *object) {
    if(auto button = dynamic_cast<TransportButton *>(object))
        return new TransportButtonAccessible(button);
    return nullptr;
}
#endif

// Three box blurs in a row come close to a Gaussian. Premultiplied ARGB, so every channel averages alike.
void boxBlur(QImage &image, int radius) {
    if(radius < 1 || image.isNull())
        return;
    if(image.format() != QImage::Format_ARGB32_Premultiplied)
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int width = image.width(), height = image.height();
    std::vector<QRgb> line(std::max(width, height));
    auto blurLine = [&](QRgb *data, int count, qsizetype stride) {
        for(int i = 0; i < count; ++i)
            line[i] = data[i * stride];
        auto at = [&](int i) { return line[std::clamp(i, 0, count - 1)]; };
        int a = 0, r = 0, g = 0, b = 0;
        for(int i = -radius; i <= radius; ++i) {
            QRgb px = at(i);
            a += qAlpha(px), r += qRed(px), g += qGreen(px), b += qBlue(px);
        }
        const int window = 2 * radius + 1;
        for(int i = 0; i < count; ++i) {
            data[i * stride] = qRgba(r / window, g / window, b / window, a / window);
            QRgb out = at(i - radius), in = at(i + radius + 1);
            a += qAlpha(in) - qAlpha(out), r += qRed(in) - qRed(out);
            g += qGreen(in) - qGreen(out), b += qBlue(in) - qBlue(out);
        }
    };
    QRgb *bits = reinterpret_cast<QRgb *>(image.bits());
    const qsizetype stride = image.bytesPerLine() / 4;
    for(int pass = 0; pass < 3; ++pass) {
        for(int y = 0; y < height; ++y)
            blurLine(bits + y * stride, width, 1);
        for(int x = 0; x < width; ++x)
            blurLine(bits + x, height, stride);
    }
}

// The cover reduced to a soft wash of its colours. Blurred small and then again after a modest upscale, so
// that the painter's bilinear stretch to the window shows no creases. Worker thread: QImage only.
QImage makeBackdrop(QImage const &cover) {
    if(cover.isNull())
        return {};
    constexpr int small = 32, large = 160;
    QImage image = cover.scaled(small, small, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    image = image.copy((image.width() - small) / 2, (image.height() - small) / 2, small, small)
                .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    boxBlur(image, 3);
    image = image.scaled(large, large, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    boxBlur(image, 10);
    return image;
}

// Elapsed and total time. Painted a character at a time so that each digit takes the width of the widest
// one: Qt 6.5 has no API for the "tnum" font feature, and proportional digits make the text shiver every
// second. The colour still comes from the stylesheet, through the palette.
class TimeLabel : public QLabel {
public:
    explicit TimeLabel(QWidget *parent) : QLabel(parent) {}

    int widthFor(QString const &text) const {
        QFontMetricsF fm(font());
        return static_cast<int>(std::ceil(textWidth(text, fm, digitWidth(fm))));
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event)
        QString str = text();
        if(str.isEmpty())
            return;
        QPainter p(this);
        p.setFont(font());
        p.setPen(palette().color(QPalette::WindowText));
        QFontMetricsF fm(font());
        qreal digit = digitWidth(fm);
        qreal free = width() - textWidth(str, fm, digit);
        qreal x = alignment() & Qt::AlignRight ? free : alignment() & Qt::AlignHCenter ? free / 2 : 0;
        qreal baseline = (height() - fm.height()) / 2 + fm.ascent();
        for(QChar c : str) {
            qreal glyph = fm.horizontalAdvance(c);
            qreal cell = c.isDigit() ? digit : glyph;
            p.drawText(QPointF(x + (cell - glyph) / 2, baseline), QString(c));
            x += cell;
        }
    }

private:
    static qreal digitWidth(QFontMetricsF const &fm) {
        qreal widest = 0;
        for(char c = '0'; c <= '9'; ++c)
            widest = std::max(widest, fm.horizontalAdvance(QChar(c)));
        return widest;
    }

    static qreal textWidth(QString const &text, QFontMetricsF const &fm, qreal digit) {
        qreal width = 0;
        for(QChar c : text)
            width += c.isDigit() ? digit : fm.horizontalAdvance(c);
        return width;
    }
};

struct Look {
    QColor widget, widgetBorder, glyph;

    bool operator==(Look const &other) const = default;
};

enum class CoverState {
    // Nothing is open.
    None,
    // The tags are still being read.
    Pending,
    // The file has no picture: the placeholder tile with a note.
    Missing,
    Image
};

// The cover (or the placeholder tile) with rounded corners and a soft shadow, kShadowPad wider than `side`
// all around. Rendered once per size so that painting it is a single blit.
QPixmap renderCover(QImage const &image, CoverState state, int side, qreal dpr, Look const &look) {
    const int pad = qRound(kShadowPad * dpr);
    const int box = qRound(side * dpr);
    QImage out(box + 2 * pad, box + 2 * pad, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QRect content(pad, pad, box, box);
    if(state == CoverState::Image) {
        // A cover that is not square keeps its shape rather than getting bars or a crop.
        QSize fitted = image.size().scaled(box, box, Qt::KeepAspectRatio);
        content =
            QRect(pad + (box - fitted.width()) / 2, pad + (box - fitted.height()) / 2, fitted.width(), fitted.height());
    }
    const qreal radius = std::min(kCoverRadius * dpr, std::min(content.width(), content.height()) / 8.0);

    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setPen(Qt::NoPen);
    {
        // A shadow is a blur anyway, so make it at a quarter of the resolution.
        constexpr int k = 4;
        QImage shadow(out.width() / k + 1, out.height() / k + 1, QImage::Format_ARGB32_Premultiplied);
        shadow.fill(Qt::transparent);
        QPainter sp(&shadow);
        sp.setRenderHint(QPainter::Antialiasing);
        sp.setPen(Qt::NoPen);
        sp.setBrush(QColor(0, 0, 0, 150));
        QRectF r(content.x() / qreal(k), (content.y() + 8 * dpr) / k, content.width() / qreal(k),
                 content.height() / qreal(k));
        sp.drawRoundedRect(r.adjusted(2 * dpr / k, 0, -2 * dpr / k, 0), radius / k, radius / k);
        sp.end();
        boxBlur(shadow, std::max(1, qRound(9 * dpr / k)));
        p.drawImage(QRectF(0, 0, shadow.width() * k, shadow.height() * k), shadow);
    }
    if(state == CoverState::Image) {
        QImage scaled = image.scaled(content.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QBrush brush(scaled);
        brush.setTransform(QTransform::fromTranslate(content.x(), content.y()));
        p.setBrush(brush);
        p.drawRoundedRect(content, radius, radius);
        // A hairline keeps a cover that is as dark as the background from losing its edge.
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 24), dpr));
        p.drawRoundedRect(QRectF(content).adjusted(dpr / 2, dpr / 2, -dpr / 2, -dpr / 2), radius, radius);
    } else {
        p.setBrush(look.widget);
        p.setPen(QPen(look.widgetBorder, dpr));
        p.drawRoundedRect(QRectF(content).adjusted(dpr / 2, dpr / 2, -dpr / 2, -dpr / 2), radius, radius);
        if(state == CoverState::Missing) {
            // The @2x icon at any scale: a large tile needs more than 72 px, and it is never enlarged past
            // its own pixels.
            QPixmap glyph(QString(kPlaceholderIcon).replace(".png", "@2x.png"));
            if(!glyph.isNull()) {
                // This painter works in device pixels.
                glyph.setDevicePixelRatio(1);
                ImageLib::recolor(glyph, look.glyph);
                qreal size = std::min<qreal>(std::max(24.0, side * 0.28) * dpr, glyph.width());
                QRectF target(content.center().x() + 1 - size / 2, content.center().y() + 1 - size / 2, size, size);
                p.drawPixmap(target, glyph, glyph.rect());
            }
        }
    }
    p.end();
    QPixmap pixmap = QPixmap::fromImage(out);
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

struct Metadata {
    QString title, artist, album;
    QImage cover, backdrop;
};

} // namespace

struct AudioView::Private {
    explicit Private(AudioView *q) : q(q) {}

    // How the column is arranged, from relayout().
    struct Layout {
        enum Mode { Tall, Wide, NoCover } mode = NoCover;
        // 0: title and two more lines; 1: title and one; 2: the title alone.
        int level = 0;
        int cover = 0;
        QRect coverRect, cardRect;
    };

    AudioView *q;
    AudioPlayer *player = nullptr;
    bool pluginMissing = false;

    QString file;
    // Bumped by every open() and stop(): a metadata job that finishes for an older value is dropped.
    int generation = 0;
    double duration = 0, position = 0;
    bool paused = true, muted = false, loop = false;
    // The user paused, and nothing has resumed since. mpv plays out the little it has buffered after a pause
    // that comes in the last moment, and then reports the end; the continuous modes must not take that as a
    // cue to start the next track.
    bool userPaused = false;
    // Finished and stopped at the end. mpv reports the last packet's time after that, a little short of the
    // duration; the bar stays full until something moves it.
    bool atEnd = false;
    int volume = 100;
    AudioPlaybackMode mode = AUDIO_MODE_SINGLE;
    bool failed = false;
    QString failure;
    QVariantMap format, playerTags;

    // While true, the previous track's title and cover are still on screen.
    bool metadataPending = false;
    bool readerDone = false;
    Metadata tags;
    QTimer graceTimer;

    CoverState coverState = CoverState::None;
    QPixmap backdrop;
    QString titleText, subtitleText, detailsText, errorText;

    bool fullscreen = false, backdropEnabled = true;
    QColor background;
    Look look;
    QMargins panelReserve;

    Layout layout;
    QRect titleSlot, lineSlots[2], backdropRect;
    // The seek bar's row in the card, the times beside it and half the gaps around it included: the wheel
    // seeks anywhere in it.
    QRect seekRow;
    QPixmap coverPixmap;
    QPoint coverPixmapPos;
    // What coverPixmap was rendered from; it is only rendered again when one of these changes.
    struct {
        int side = 0;
        qreal dpr = 0;
        CoverState state = CoverState::None;
        qint64 image = 0, look = 0;
    } coverKey;
    qint64 lookSerial = 0;

    QWidget *card = nullptr;
    QLabel *title = nullptr, *subtitle = nullptr, *details = nullptr, *errorLabel = nullptr;
    QLabel *pluginError = nullptr;
    TimeLabel *elapsed = nullptr, *total = nullptr;
    MediaSlider *seekBar = nullptr, *volumeSlider = nullptr;
    TransportButton *modeButton = nullptr, *prevButton = nullptr, *playButton = nullptr, *nextButton = nullptr;
    TransportButton *muteButton = nullptr;

    void createWidgets();
    QLabel *createLabel(QString const &name, QFont const &font);
    TransportButton *createButton(QString const &name, QString const &action, QString const &icon, bool primary);
    void setButtonText(TransportButton *button, QString const &name, QString const &toolTip);
    bool ensurePlayer();
    void attachPlayer(AudioPlayer *player);

    void beginFile(QString const &path);
    void startMetadataJob();
    void onMetadataLoaded(int forGeneration, Metadata const &result);
    void commitMetadata();
    void clear();
    void showPluginError();
    void setError(QString const &message);

    QString timeText(double seconds) const;
    void showPosition(double seconds);
    void showDuration();
    void setVolume(int value, bool byUser);
    void setMuted(bool mode);
    void updatePlayButton();
    void updateMuteButton();
    void updateModeButton();
    void updateToolTips();
    void readSettings();

    void refreshTexts();
    void placeTexts();
    void setElided(QLabel *label, QString const &text);
    int cardHeight(int level) const;
    int lineHeight() const;
    bool fit(QRect const &full, QRect const &cardArea, int level, bool wide, Layout &out, bool force = false) const;
    void relayout();
    void layoutCard();
    void updateCover();
};

void AudioView::Private::createWidgets() {
#if QT_CONFIG(accessibility)
    static bool const factoryInstalled = [] {
        QAccessible::installFactory(createAccessible);
        return true;
    }();
    Q_UNUSED(factoryInstalled)
#endif
    QFont base = q->font();
    card = new QWidget(q);
    card->setObjectName("AudioCard");
    card->setAttribute(Qt::WA_StyledBackground);
    card->setFocusPolicy(Qt::NoFocus);
    card->setMouseTracking(true);
    // For the wheel around the seek bar; see AudioView::eventFilter().
    card->installEventFilter(q);

    title = createLabel("AudioTitle", scaledFont(base, 1.5, QFont::DemiBold));
    subtitle = createLabel("AudioSubtitle", scaledFont(base, 1.08));
    details = createLabel("AudioDetails", scaledFont(base, 0.92));
    errorLabel = createLabel("AudioError", scaledFont(base, 0.92));

    QFont timeFont = scaledFont(base, 0.92);
    elapsed = new TimeLabel(card);
    elapsed->setObjectName("AudioElapsed");
    elapsed->setMouseTracking(true);
    elapsed->setFont(timeFont);
    elapsed->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    total = new TimeLabel(card);
    total->setObjectName("AudioDuration");
    total->setMouseTracking(true);
    total->setFont(timeFont);
    total->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    seekBar = new MediaSlider(card);
    seekBar->setObjectName("AudioSeekBar");
    seekBar->setAccessibleName(AudioView::tr("Position"));
    seekBar->setWheelStep(kSeekWheelStep);
    seekBar->setDragInterval(kSeekDragInterval);
    seekBar->setToolTipText([this](double seconds) { return formatTime(seconds, duration >= 3600); });
    connect(seekBar, &MediaSlider::valueRequested, q, [this](double seconds) {
        if(!player || file.isEmpty())
            return;
        player->seek(seconds);
        atEnd = false;
        position = seconds;
        elapsed->setText(timeText(seconds));
    });

    modeButton = createButton("AudioModeButton", "cycleAudioMode", modeIcon(mode), false);
    prevButton = createButton("AudioPrevButton", "prevImage", kButtonIcons + "prev-track24.png", false);
    playButton = createButton("AudioPlayButton", "pauseVideo", kButtonIcons + "play32.png", true);
    nextButton = createButton("AudioNextButton", "nextImage", kButtonIcons + "next-track24.png", false);
    muteButton = createButton("AudioMuteButton", "toggleMute", kButtonIcons + "volume24.png", false);

    volumeSlider = new MediaSlider(card);
    volumeSlider->setObjectName("AudioVolume");
    volumeSlider->setAccessibleName(AudioView::tr("Volume"));
    volumeSlider->setMaximum(100);
    volumeSlider->setWheelStep(kVolumeStep);
    volumeSlider->setToolTipText([](double value) { return AudioView::tr("Volume: %1%").arg(qRound(value)); });
    connect(volumeSlider, &MediaSlider::valueRequested, q, [this](double value) { setVolume(qRound(value), true); });

    pluginError = new QLabel(q);
    pluginError->setObjectName("AudioPluginError");
    pluginError->setAlignment(Qt::AlignCenter);
    pluginError->setTextFormat(Qt::PlainText);
    pluginError->setWordWrap(true);
    pluginError->setFocusPolicy(Qt::NoFocus);
    pluginError->setMouseTracking(true);
    pluginError->hide();

    graceTimer.setSingleShot(true);
    graceTimer.setInterval(kMetadataGrace);
    connect(&graceTimer, &QTimer::timeout, q, [this] { commitMetadata(); });
}

QLabel *AudioView::Private::createLabel(QString const &name, QFont const &font) {
    auto label = new QLabel(card);
    label->setObjectName(name);
    label->setFont(font);
    // Tags are arbitrary text; "<b>" in a title is not markup.
    label->setTextFormat(Qt::PlainText);
    label->setFocusPolicy(Qt::NoFocus);
    label->setMouseTracking(true);
    return label;
}

TransportButton *AudioView::Private::createButton(QString const &name, QString const &action, QString const &icon,
                                                  bool primary) {
    auto button = new TransportButton(action, icon, primary ? kPlayButton : kButton, card);
    button->setObjectName(name);
    // The stylesheet's hook. Not the accessible name, as elsewhere in qimgv: that is what a screen reader says.
    button->setProperty("audioButton", primary ? "primary" : "small");
    return button;
}

void AudioView::Private::setButtonText(TransportButton *button, QString const &name, QString const &toolTip) {
    button->setAccessibleName(name);
    button->setToolTip(toolTip);
}

bool AudioView::Private::ensurePlayer() {
    if(player)
        return true;
    using CreateAudioPlayerFn = AudioPlayer *(*)();
    auto create = reinterpret_cast<CreateAudioPlayerFn>(PlayerPlugin::resolve("CreateAudioPlayer"));
    if(!create)
        return false;
    AudioPlayer *created = create();
    if(!created)
        return false;
    attachPlayer(created);
    return true;
}

void AudioView::Private::attachPlayer(AudioPlayer *newPlayer) {
    delete player;
    player = newPlayer;
    player->setParent(q);
    // By name: the plugin's AudioPlayer meta-object is its own copy (see audioplayer.h).
    connect(player, SIGNAL(fileLoaded()), q, SLOT(onFileLoaded()));
    connect(player, SIGNAL(durationChanged(double)), q, SLOT(onDurationChanged(double)));
    connect(player, SIGNAL(positionChanged(double)), q, SLOT(onPositionChanged(double)));
    connect(player, SIGNAL(pausedChanged(bool)), q, SLOT(onPausedChanged(bool)));
    connect(player, SIGNAL(metadataChanged(QVariantMap)), q, SLOT(onMetadataChanged(QVariantMap)));
    connect(player, SIGNAL(formatChanged(QVariantMap)), q, SLOT(onFormatChanged(QVariantMap)));
    connect(player, SIGNAL(playbackFinished()), q, SLOT(onPlaybackFinished()));
    connect(player, SIGNAL(failed(QString)), q, SLOT(onFailed(QString)));
}

void AudioView::Private::beginFile(QString const &path) {
    file = path;
    ++generation;
    duration = 0;
    position = 0;
    paused = false;
    userPaused = false;
    atEnd = false;
    failed = false;
    failure.clear();
    format.clear();
    playerTags.clear();
    readerDone = false;
    tags = Metadata();
    seekBar->setMaximum(0);
    seekBar->setValue(0);
    seekBar->setEnabled(true);
    playButton->setEnabled(true);
    showDuration();
    showPosition(0);
    updatePlayButton();
    card->show();
    // Wait a moment for the tags. Between two tracks the previous one's title and cover stay up meanwhile;
    // coming from anything else the column stays empty, rather than showing a placeholder and the file name
    // that the tags replace an instant later.
    metadataPending = true;
    graceTimer.start();
    startMetadataJob();
}

void AudioView::Private::startMetadataJob() {
    QString path = file;
    int forGeneration = generation;
    int maxSide = static_cast<int>(std::ceil(kCoverMax * q->devicePixelRatioF()));
    QPointer<AudioView> view(q);
    QThreadPool::globalInstance()->start([path, forGeneration, maxSide, view] {
        Metadata result;
        AudioMetadata meta = AudioMetadataReader::read(path, AudioMetadataReader::Tags);
        result.title = meta.title.simplified();
        result.artist = (meta.artist.isEmpty() ? meta.albumArtist : meta.artist).simplified();
        result.album = meta.album.simplified();
        result.cover = AudioMetadataReader::loadCover(path, QSize(maxSide, maxSide));
        result.backdrop = makeBackdrop(result.cover);
        // Posted to the application object, which outlives any view; the view itself may be gone by the
        // time this runs, and the QPointer is only checked back on the GUI thread.
        QMetaObject::invokeMethod(
            qApp,
            [view, forGeneration, result] {
                // One read of the QPointer, checked: GCC's -Wnull-dereference does not connect two calls to
                // its data() at -O3.
                if(AudioView *alive = view.data())
                    alive->d->onMetadataLoaded(forGeneration, result);
            },
            Qt::QueuedConnection);
    });
}

void AudioView::Private::onMetadataLoaded(int forGeneration, Metadata const &result) {
    if(forGeneration != generation)
        return;
    readerDone = true;
    tags = result;
    backdrop = result.backdrop.isNull() ? QPixmap() : QPixmap::fromImage(result.backdrop);
    commitMetadata();
}

void AudioView::Private::commitMetadata() {
    graceTimer.stop();
    metadataPending = false;
    if(file.isEmpty())
        coverState = CoverState::None;
    else if(!readerDone)
        coverState = CoverState::Pending;
    else
        coverState = tags.cover.isNull() ? CoverState::Missing : CoverState::Image;
    if(coverState != CoverState::Image)
        backdrop = QPixmap();
    refreshTexts();
    updateCover();
    q->update();
}

void AudioView::Private::clear() {
    file.clear();
    ++generation;
    graceTimer.stop();
    metadataPending = false;
    readerDone = false;
    tags = Metadata();
    backdrop = QPixmap();
    duration = 0;
    position = 0;
    paused = true;
    userPaused = false;
    atEnd = false;
    failed = false;
    failure.clear();
    format.clear();
    playerTags.clear();
    seekBar->setMaximum(0);
    showDuration();
    showPosition(0);
    updatePlayButton();
    commitMetadata();
    // Closed, e.g. the file was deleted: an empty view, like the image viewer's, rather than controls for
    // nothing.
    card->hide();
    pluginError->hide();
}

void AudioView::Private::showPluginError() {
    pluginMissing = true;
    pluginError->setText(PlayerPlugin::loadError());
    pluginError->show();
    card->hide();
    coverPixmap = QPixmap();
    relayout();
    q->update();
}

void AudioView::Private::setError(QString const &message) {
    failed = true;
    failure = message;
    paused = true;
    showDuration();
    updatePlayButton();
    playButton->setEnabled(false);
    seekBar->setEnabled(false);
    // An error under the previous track's title would point at the wrong file.
    if(metadataPending)
        commitMetadata();
    else
        refreshTexts();
}

QString AudioView::Private::timeText(double seconds) const {
    // Nothing to count for a file that will not play, nor before mpv has said how long it is.
    if(failed || (duration <= 0 && seconds <= 0))
        return {};
    return formatTime(seconds, duration >= 3600);
}

void AudioView::Private::showPosition(double seconds) {
    position = seconds;
    seekBar->setValue(seconds);
    if(!seekBar->isDragging())
        elapsed->setText(timeText(seconds));
}

void AudioView::Private::showDuration() {
    QString oldPattern = timePattern(seekBar->maximum());
    seekBar->setMaximum(duration);
    total->setText(timeText(duration));
    elapsed->setText(timeText(position));
    if(timePattern(duration) != oldPattern)
        layoutCard();
}

void AudioView::Private::setVolume(int value, bool byUser) {
    volume = std::clamp(value, 0, 100);
    if(player)
        player->setVolume(volume);
    settings->setVolume(volume);
    volumeSlider->setValue(volume);
    // Reaching for the volume while muted means "I want to hear this".
    if(byUser && muted && volume > 0)
        setMuted(false);
}

void AudioView::Private::setMuted(bool mode) {
    muted = mode;
    if(player)
        player->setMuted(muted);
    updateMuteButton();
}

void AudioView::Private::updatePlayButton() {
    playButton->setIconPath(kButtonIcons + (paused ? "play32.png" : "pause32.png"));
    QString text = paused ? AudioView::tr("Play") : AudioView::tr("Pause");
    setButtonText(playButton, text, text);
}

void AudioView::Private::updateMuteButton() {
    muteButton->setIconPath(kButtonIcons + (muted ? "volume-muted24.png" : "volume24.png"));
    QString text = muted ? AudioView::tr("Unmute") : AudioView::tr("Mute");
    setButtonText(muteButton, text, text);
    volumeSlider->setFillFaded(muted);
}

void AudioView::Private::updateModeButton() {
    modeButton->setIconPath(modeIcon(mode));
    QString text = AudioView::tr("Playback: %1").arg(modeName(mode));
    setButtonText(modeButton, text, withShortcut(text, "cycleAudioMode"));
    // Every mode but the default one is "on". A grey accent cannot say that, so use the brightest text.
    ColorScheme const &colors = settings->colorScheme();
    QColor on = colors.accent;
    if(on.hsvSaturation() < 40 || contrast(on, colors.widget) < 2.5)
        on = colors.text_hc2;
    modeButton->setColor(mode == AUDIO_MODE_SINGLE ? colors.icons : on);
}

void AudioView::Private::updateToolTips() {
    setButtonText(prevButton, AudioView::tr("Previous"), withShortcut(AudioView::tr("Previous"), "prevImage"));
    setButtonText(nextButton, AudioView::tr("Next"), withShortcut(AudioView::tr("Next"), "nextImage"));
    updateModeButton();
    updatePlayButton();
    updateMuteButton();
}

void AudioView::Private::readSettings() {
    ColorScheme const &colors = settings->colorScheme();
    // Exactly what ImageViewerV2 and VideoPlayerInitProxy paint, so that switching to an audio file does not
    // change the backdrop.
    if(fullscreen) {
        background = colors.background_fullscreen;
        background.setAlphaF(1.0);
    } else {
        background = colors.background;
        background.setAlphaF(settings->backgroundOpacity());
    }
    backdropEnabled = settings->audioBackdrop();

    // settingsChanged comes for any setting; render the cover again only for a new look.
    Look newLook{colors.widget, colors.widget_border, colors.icons};
    if(newLook != look) {
        look = newLook;
        ++lookSerial;
    }

    // 3:1, the usual floor for telling two states of a control apart: played from unplayed, the volume from
    // the rest. Some schemes' accents are much closer to the track than that (Black's is a grey).
    QColor track = mix(colors.widget, colors.text, 0.2);
    seekBar->setColors(track, legibleOn(colors.accent, track, 3.0), colors.text_hc2);
    volumeSlider->setColors(track, legibleOn(colors.text_lc, track, 3.0), colors.text_hc2);
    QPalette palette = errorLabel->palette();
    bool darkCard = colors.widget.valueF() <= 0.45f;
    palette.setColor(QPalette::WindowText, darkCard ? QColor(0xe5, 0x73, 0x73) : QColor(0xc6, 0x28, 0x28));
    errorLabel->setPalette(palette);

    panelReserve = QMargins();
    if(settings->panelEnabled() && !settings->panelPinned() && (fullscreen || !settings->panelFullscreenOnly())) {
        // The unpinned panel slides in when the mouse enters its strip, so controls inside that strip would
        // summon it over themselves.
        int thickness = settings->panelPreviewsSize() + kPanelFrame;
        switch(settings->panelPosition()) {
        case PANEL_TOP:
            panelReserve.setTop(thickness);
            break;
        case PANEL_BOTTOM:
            panelReserve.setBottom(thickness);
            break;
        case PANEL_LEFT:
            panelReserve.setLeft(thickness);
            break;
        case PANEL_RIGHT:
            panelReserve.setRight(thickness);
            break;
        }
    }
    updateToolTips();
    relayout();
    q->update();
}

void AudioView::Private::refreshTexts() {
    if(metadataPending)
        return;
    QString name, artist, album;
    // The reader knows the tag formats better; mpv's view of them is the fallback for what it cannot parse.
    if(readerDone && (!tags.title.isEmpty() || !tags.artist.isEmpty() || !tags.album.isEmpty())) {
        name = tags.title;
        artist = tags.artist;
        album = tags.album;
    } else {
        name = playerTags.value("title").toString().simplified();
        artist = playerTags.value("artist").toString().simplified();
        if(artist.isEmpty())
            artist = playerTags.value("album_artist").toString().simplified();
        album = playerTags.value("album").toString().simplified();
    }
    if(name.isEmpty() && !file.isEmpty())
        name = QFileInfo(file).completeBaseName();
    titleText = name;
    QStringList line;
    if(!artist.isEmpty())
        line << artist;
    if(!album.isEmpty())
        line << album;
    subtitleText = line.join(QStringLiteral(" · "));
    detailsText = formatLine(format);
    if(!failed)
        errorText.clear();
    else if(failure.isEmpty())
        errorText = AudioView::tr("Could not play this file");
    else
        errorText = AudioView::tr("Could not play this file: %1").arg(failure);
    placeTexts();
}

void AudioView::Private::setElided(QLabel *label, QString const &text) {
    QString elided = label->fontMetrics().elidedText(text, Qt::ElideRight, label->width());
    label->setText(elided);
    label->setToolTip(elided == text ? QString() : text);
}

// Puts the lines that have something to say into the slots the layout has room for: an error first, as it
// matters most, then artist and album, then the format.
void AudioView::Private::placeTexts() {
    QList<QPair<QLabel *, QString>> lines;
    if(!errorText.isEmpty())
        lines.append({errorLabel, errorText});
    if(!subtitleText.isEmpty())
        lines.append({subtitle, subtitleText});
    // A file that plays gets a format line once mpv has worked out the codec, a moment in. Its slot is kept
    // meanwhile, so that the line does not push the title up when it comes.
    if(errorText.isEmpty() && (!detailsText.isEmpty() || !file.isEmpty()))
        lines.append({details, detailsText});
    int room = 0;
    while(room < 2 && !lineSlots[room].isEmpty())
        ++room;
    int shown = std::min<int>(room, lines.size());
    // Fewer lines than slots: centre what there is instead of leaving a hole above the seek bar.
    int offset = (room - shown) * (kLineGap + lineHeight()) / 2;
    title->setGeometry(titleSlot.translated(0, offset));
    setElided(title, titleText);
    for(QLabel *label : {errorLabel, subtitle, details}) {
        int i = 0;
        while(i < shown && lines[i].first != label)
            ++i;
        if(i < shown) {
            label->setGeometry(lineSlots[i].translated(0, offset));
            setElided(label, lines[i].second);
        }
        label->setVisible(i < shown);
    }
}

int AudioView::Private::lineHeight() const {
    return std::max(subtitle->fontMetrics().height(), details->fontMetrics().height());
}

int AudioView::Private::cardHeight(int level) const {
    int text = title->fontMetrics().height();
    if(level < 2)
        text += kLineGap + lineHeight();
    if(level < 1)
        text += kLineGap + lineHeight();
    return kPadV + text + kTextToSeek + kSeekHeight + kSeekToTransport + kPlayButton + kPadV;
}

// Arranges the column for `level` within `full`, keeping the card itself inside `cardArea`: the controls must
// stay out of the reserved strips, a cover there does no harm. False when the card does not fit at all.
bool AudioView::Private::fit(QRect const &full, QRect const &cardArea, int level, bool wide, Layout &out,
                             bool force) const {
    const int cardH = cardHeight(level);
    if(!force && (cardH > cardArea.height() || cardArea.width() < kCardMinWidth))
        return false;
    const QPoint centre = q->rect().center();
    // The start of a run of `size` centred on `c`, moved into [lo, hi) if that takes it out.
    auto place = [](int c, int size, int lo, int hi) { return std::max(lo, std::min(c - size / 2, hi - size)); };
    const int areaLeft = cardArea.left(), areaRight = cardArea.left() + cardArea.width();
    const int areaTop = cardArea.top(), areaBottom = cardArea.top() + cardArea.height();
    const int fullTop = full.top(), fullBottom = full.top() + full.height();
    Layout l;
    l.level = level;
    if(wide) {
        // The cover to the left of the card and centred on it.
        int cardY = place(centre.y(), cardH, areaTop, areaBottom);
        int middle = cardY + cardH / 2;
        int room = 2 * std::min(middle - fullTop, fullBottom - middle);
        int side = std::min({kCoverMax, room, cardArea.width() - kCoverGap - kCardWidth});
        if(side < kCoverMin)
            side = std::min({kCoverMax, room, cardArea.width() - kCoverGap - kCardMinWidth});
        if(side >= kCoverMin) {
            int cardW = std::min(cardArea.width() - side - kCoverGap, kCardMaxWidth);
            int x = place(centre.x(), side + kCoverGap + cardW, areaLeft, areaRight);
            l.mode = Layout::Wide;
            l.cover = side;
            l.coverRect = QRect(x, middle - side / 2, side, side);
            l.cardRect = QRect(x + side + kCoverGap, cardY, cardW, cardH);
            out = l;
            return true;
        }
    }
    // The cover above the card.
    int side = std::min({kCoverMax, full.width(), areaBottom - fullTop - kCoverGap - cardH});
    if(side >= kCoverMin) {
        int cardW = std::min({std::max(side, kCardWidth), cardArea.width(), kCardMaxWidth});
        int cardX = place(centre.x(), cardW, areaLeft, areaRight);
        int middle = cardX + cardW / 2;
        side = std::min(side, 2 * std::min(middle - full.left(), full.left() + full.width() - middle));
        int blockH = side + kCoverGap + cardH;
        int top = place(centre.y(), blockH, std::max(fullTop, areaTop - side - kCoverGap), areaBottom);
        l.mode = Layout::Tall;
        l.cover = side;
        l.coverRect = QRect(middle - side / 2, top, side, side);
        l.cardRect = QRect(cardX, top + side + kCoverGap, cardW, cardH);
        out = l;
        return true;
    }
    int cardW = std::min(cardArea.width(), kCardMaxWidth);
    l.cardRect = QRect(place(centre.x(), cardW, areaLeft, areaRight), place(centre.y(), cardH, areaTop, areaBottom),
                       cardW, cardH);
    out = l;
    return true;
}

void AudioView::Private::relayout() {
    QRect view = q->rect();
    QRect full = view.marginsRemoved(QMargins(kMargin, kMargin, kMargin, kMargin));
    if(pluginMissing) {
        pluginError->setGeometry(full);
        return;
    }
    QRect panelSafe = full.marginsRemoved(panelReserve);
    QMargins reserve = panelReserve;
    reserve.setBottom(std::max(reserve.bottom(), kMessageReserve));
    QRect safe = full.marginsRemoved(reserve);
    bool wide = view.height() < kWideMaxHeight && view.width() > kWideAspect * view.height();

    // Every line of text matters more than the message's strip, but less than staying out of the panel's;
    // the whole view is the last resort.
    Layout l;
    bool found = false;
    for(int level = 0; level <= 2 && !found; ++level)
        found = fit(full, safe, level, wide, l) || fit(full, panelSafe, level, wide, l);
    for(int level = 0; level <= 2 && !found; ++level)
        found = fit(full, full, level, wide, l);
    // Still too small: give up the margins too, so that a short window cuts nothing off the card.
    if(!found)
        fit(view, view, 2, wide, l, true);
    layout = l;

    card->setGeometry(layout.cardRect);
    // The backdrop is square: stretch it over the longer side and let the view crop it.
    int side = std::max(view.width(), view.height());
    backdropRect = QRect(0, 0, side, side);
    backdropRect.moveCenter(view.center());
    layoutCard();
    updateCover();
    q->update();
}

void AudioView::Private::layoutCard() {
    const int width = layout.cardRect.width();
    const int x0 = kPadH;
    const int inner = std::max(0, width - 2 * kPadH);
    Qt::Alignment align = (layout.mode == Layout::Wide ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignVCenter;
    for(QLabel *label : {title, subtitle, details, errorLabel})
        label->setAlignment(align);

    int y = kPadV;
    const int titleH = title->fontMetrics().height();
    titleSlot = QRect(x0, y, inner, titleH);
    y += titleH;
    for(int i = 0; i < 2; ++i) {
        lineSlots[i] = QRect();
        if(layout.level < 2 - i) {
            lineSlots[i] = QRect(x0, y + kLineGap, inner, lineHeight());
            y += kLineGap + lineHeight();
        }
    }
    y += kTextToSeek;

    // Both time labels as wide as the longest time this track can show, so the bar stays centred and still.
    QString pattern = timePattern(duration);
    int timeW = std::max(elapsed->widthFor(pattern), total->widthFor(pattern));
    elapsed->setGeometry(x0, y, timeW, kSeekHeight);
    total->setGeometry(x0 + inner - timeW, y, timeW, kSeekHeight);
    int seekX = x0 + timeW + kTimeGap;
    seekBar->setGeometry(seekX, y, std::max(0, x0 + inner - timeW - kTimeGap - seekX), kSeekHeight);
    seekRow = QRect(x0, y - kTextToSeek / 2, inner, kTextToSeek / 2 + kSeekHeight + kSeekToTransport);
    y += kSeekHeight + kSeekToTransport;

    // Previous, play and next centred on the card; the mode on the left edge, mute and volume on the right.
    const int buttonY = y + (kPlayButton - kButton) / 2;
    const int centre = width / 2;
    playButton->setGeometry(centre - kPlayButton / 2, y, kPlayButton, kPlayButton);
    prevButton->setGeometry(playButton->x() - kTransportGap - kButton, buttonY, kButton, kButton);
    nextButton->setGeometry(playButton->x() + kPlayButton + kTransportGap, buttonY, kButton, kButton);
    modeButton->setGeometry(x0, buttonY, kButton, kButton);
    // A window too small for the card's minimum width: the mode and mute buttons would sit on top of previous
    // and next. Those matter more, and the other two still have their keys.
    const bool sides = x0 + kButton + kTransportGap <= prevButton->x() &&
                       nextButton->x() + kButton + kTransportGap <= x0 + inner - kButton;
    modeButton->setVisible(sides);
    muteButton->setVisible(sides);
    int rightSpace = x0 + inner - (nextButton->x() + kButton + kVolumeGap);
    int volumeW = std::min(rightSpace - kButton, kVolumeMax);
    if(sides && volumeW >= kVolumeMin) {
        volumeSlider->setGeometry(x0 + inner - volumeW, y + (kPlayButton - kSeekHeight) / 2, volumeW, kSeekHeight);
        volumeSlider->show();
        muteButton->setGeometry(volumeSlider->x() - kButton, buttonY, kButton, kButton);
    } else {
        volumeSlider->hide();
        muteButton->setGeometry(x0 + inner - kButton, buttonY, kButton, kButton);
    }
    placeTexts();
}

void AudioView::Private::updateCover() {
    if(pluginMissing || coverState == CoverState::None || layout.mode == Layout::NoCover) {
        coverPixmap = QPixmap();
        coverKey = {};
        return;
    }
    qreal dpr = q->devicePixelRatioF();
    qint64 image = coverState == CoverState::Image ? tags.cover.cacheKey() : 0;
    if(coverKey.side == layout.cover && coverKey.dpr == dpr && coverKey.state == coverState &&
       coverKey.image == image && coverKey.look == lookSerial && !coverPixmap.isNull()) {
        coverPixmapPos = layout.coverRect.topLeft() - QPoint(kShadowPad, kShadowPad);
        return;
    }
    coverPixmap = renderCover(tags.cover, coverState, layout.cover, dpr, look);
    coverPixmapPos = layout.coverRect.topLeft() - QPoint(kShadowPad, kShadowPad);
    coverKey = {layout.cover, dpr, coverState, image, lookSerial};
}

//------------------------------------------------------------------------------

AudioView::AudioView(QWidget *parent) : QWidget(parent), d(new Private(this)) {
    setFocusPolicy(Qt::NoFocus);
    // Qt drops a hover move at the first widget under the cursor that does not track the mouse. Tracking and
    // then ignoring them (as QWidget and QLabel do) passes them on to ViewerWidget and the panel's trigger.
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    d->createWidgets();
    d->volume = settings->volume();
    d->volumeSlider->setValue(d->volume);
    d->readSettings();
    connect(settings, &Settings::settingsChanged, this, [this] { d->readSettings(); });
}

AudioView::~AudioView() {
    // Not left to QWidget's destructor, which deletes the children when this is no longer an AudioView:
    // nothing the player sends on its way out may reach these slots.
    delete d->player;
}

bool AudioView::open(QString const &file) {
    if(!d->ensurePlayer()) {
        d->file = file;
        d->showPluginError();
        return false;
    }
    if(d->pluginMissing) {
        d->pluginMissing = false;
        d->pluginError->hide();
        d->card->show();
    }
    d->beginFile(file);
    // Video shares the volume, so it may have changed since the last track.
    d->volume = std::clamp(settings->volume(), 0, 100);
    d->volumeSlider->setValue(d->volume);
    d->player->setVolume(d->volume);
    d->player->setMuted(d->muted);
    d->player->setLoop(d->loop);
    if(!d->player->open(file)) {
        d->setError(QString());
        return false;
    }
    return true;
}

void AudioView::stop() {
    if(d->player)
        d->player->stop();
    d->clear();
}

void AudioView::unload() {
    if(d->player)
        d->player->unload();
    d->clear();
}

QString AudioView::currentFile() const {
    return d->file;
}

void AudioView::setPaused(bool paused) {
    if(!d->player || d->file.isEmpty() || d->failed)
        return;
    d->paused = paused;
    d->userPaused = paused;
    if(!paused)
        d->atEnd = false;
    d->player->setPaused(paused);
    d->updatePlayButton();
}

void AudioView::togglePaused() {
    setPaused(!d->paused);
}

bool AudioView::isPaused() const {
    return d->paused;
}

void AudioView::seek(double seconds) {
    if(!d->player || d->file.isEmpty() || d->failed)
        return;
    if(d->duration > 0)
        seconds = std::clamp(seconds, 0.0, d->duration);
    d->player->seek(seconds);
    d->atEnd = false;
    d->showPosition(seconds);
}

void AudioView::seekRelative(double seconds) {
    if(!d->player || d->file.isEmpty() || d->failed)
        return;
    d->player->seekRelative(seconds);
    d->atEnd = false;
    if(d->duration > 0)
        d->showPosition(std::clamp(d->position + seconds, 0.0, d->duration));
}

void AudioView::toggleMute() {
    d->setMuted(!d->muted);
}

bool AudioView::isMuted() const {
    return d->muted;
}

void AudioView::volumeUp() {
    d->setVolume(d->volume + kVolumeStep, true);
}

void AudioView::volumeDown() {
    d->setVolume(d->volume - kVolumeStep, false);
}

void AudioView::setLoop(bool loop) {
    d->loop = loop;
    if(d->player)
        d->player->setLoop(loop);
}

void AudioView::setPlaybackMode(AudioPlaybackMode mode) {
    d->mode = mode;
    d->updateModeButton();
}

void AudioView::onFullscreenModeChanged(bool fullscreen) {
    d->fullscreen = fullscreen;
    d->readSettings();
}

void AudioView::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QPainter p(this);
    p.fillRect(rect(), d->background);
    if(d->pluginMissing)
        return;
    if(d->backdropEnabled && d->coverState == CoverState::Image && !d->backdrop.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setOpacity(kBackdropOpacity);
        // Tints the background without making it any more opaque: a translucent window stays exactly as
        // translucent as the user set it, as it does under an image.
        p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
        p.drawPixmap(d->backdropRect, d->backdrop);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setOpacity(1.0);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    }
    if(d->coverPixmap.isNull())
        return;
    // Moved to a screen with another scale: render the cover again for it, after this frame.
    if(d->coverPixmap.devicePixelRatio() != devicePixelRatioF())
        QMetaObject::invokeMethod(
            this,
            [this] {
                d->updateCover();
                update();
            },
            Qt::QueuedConnection);
    p.drawPixmap(d->coverPixmapPos, d->coverPixmap);
}

void AudioView::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    d->relayout();
}

// The wheel seeks anywhere along the seek bar's row, over the times beside it and in the gaps around it: just
// off the bar it would otherwise change the file, and in Single-file mode stop the music for an image.
bool AudioView::eventFilter(QObject *watched, QEvent *event) {
    if(watched == d->card && event->type() == QEvent::Wheel) {
        auto wheel = static_cast<QWheelEvent *>(event);
        if(d->seekRow.contains(wheel->position().toPoint()) && d->seekBar->takeWheel(wheel))
            return true;
    }
    return QWidget::eventFilter(watched, event);
}

void AudioView::onFileLoaded() {
    if(d->failed)
        return;
    d->playButton->setEnabled(true);
    d->seekBar->setEnabled(true);
}

void AudioView::onDurationChanged(double seconds) {
    d->duration = std::max(0.0, seconds);
    d->showDuration();
}

void AudioView::onPositionChanged(double seconds) {
    if(d->atEnd)
        return;
    if(d->seekBar->isDragging()) {
        d->position = seconds;
        return;
    }
    d->showPosition(seconds);
}

void AudioView::onPausedChanged(bool paused) {
    // Playing again, the user's pause is over, whoever ended it.
    if(!paused)
        d->userPaused = false;
    d->paused = paused;
    d->updatePlayButton();
}

void AudioView::onMetadataChanged(QVariantMap tags) {
    d->playerTags = tags;
    d->refreshTexts();
}

void AudioView::onFormatChanged(QVariantMap format) {
    d->format = format;
    d->refreshTexts();
}

void AudioView::onPlaybackFinished() {
    // Single-file mode stops here: show the whole bar played and offer Play to start over.
    d->paused = true;
    d->updatePlayButton();
    if(d->duration > 0)
        d->showPosition(d->duration);
    d->atEnd = true;
    // Paused by the user, who only reached the end because mpv played out what it had buffered (or who
    // seeked there while paused): the track stays where it is rather than the next one starting to play.
    if(d->userPaused)
        return;
    emit playbackFinished();
}

void AudioView::onFailed(QString message) {
    d->setError(message);
    emit playbackFailed();
}

void AudioView::setPlayerForTesting(AudioPlayer *player) {
    d->attachPlayer(player);
}
