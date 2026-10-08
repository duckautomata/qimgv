#include "audioplayermpv.h"
#include "qthelper.hpp"

#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <algorithm>
#include <array>
#include <cmath>

namespace {

// reply_userdata of the observed properties, and the kind of a one-off read of one of them.
enum Property : uint64_t {
    kDuration = 1,
    kTimePos,
    kPause,
    kEofReached,
    kMetadata,
    kAudioParams,
    kCodecName,
    kCodecDescription,
    kFileFormat
};

struct Observed {
    Property id;
    char const *name;
    mpv_format format;
};

constexpr Observed kObserved[] = {
    {kDuration, "duration", MPV_FORMAT_DOUBLE},
    {kTimePos, "time-pos", MPV_FORMAT_DOUBLE},
    {kPause, "pause", MPV_FORMAT_FLAG},
    {kEofReached, "eof-reached", MPV_FORMAT_FLAG},
    {kMetadata, "metadata", MPV_FORMAT_NODE},
    {kAudioParams, "audio-params", MPV_FORMAT_NODE},
    {kCodecName, "audio-codec-name", MPV_FORMAT_STRING},
    {kCodecDescription, "audio-codec", MPV_FORMAT_STRING},
    {kFileFormat, "file-format", MPV_FORMAT_STRING},
};

// The other kinds of reply. An asynchronous request carries its kind in the low bits of reply_userdata, and
// above them the generation (see AudioPlayerMpv::forget()) of the file it was made for.
enum Request : uint64_t {
    kUntracked = 0,
    kBitrate = 16,
    kDemuxBitrate,
    kLoadFile,
    kStop,
    kPauseSet,
    kVolumeSet,
    kMuteSet,
    kSeek
};
constexpr int kKindBits = 5;
constexpr uint64_t kKindMask = (uint64_t(1) << kKindBits) - 1;

// mpv hands out playlist entry ids from 1. kAnyEntry is a "loadfile" that did not say which entry it
// made (libmpv before 0.33): the next file to start is then taken to be the one asked for.
constexpr int64_t kNoEntry = 0;
constexpr int64_t kAnyEntry = -1;

// audio-bitrate is measured over recently read packets and swings with VBR content, so it is read once, about
// a second of playback after the file started or was last seeked in.
constexpr double kBitrateAfter = 1.0;

// A local file is closed within milliseconds; this only bounds how long a wedged mpv can hold the UI.
constexpr qint64 kStopTimeoutMs = 2000;

// Tracker modules (ffmpeg's libmodplug demuxer, which goes by the extension alone) and game music (libgme, by
// a four-byte tag). Both score a file under 16 KiB -- most chiptunes -- only 24 or 25, under mpv's default
// demuxer-lavf-probescore of 26, so mpv refuses it. libopenmpt, which other ffmpeg builds use for modules,
// is sure of them anyway.
constexpr std::array kChiptuneSuffixes = {"669", "amf", "ams", "dbm", "dmf", "dsm",  "far", "it",  "mdl", "med", "mod",
                                          "mt2", "mtm", "okt", "psm", "ptm", "s3m",  "stm", "ult", "umx", "xm",  "ay",
                                          "gbs", "gym", "hes", "kss", "nsf", "nsfe", "sap", "spc", "vgm", "vgz"};
constexpr char const *kChiptuneOptions = "demuxer-lavf-probescore=24";

bool isChiptune(QString const &file) {
    const QString suffix = QFileInfo(file).suffix();
    return std::any_of(kChiptuneSuffixes.begin(), kChiptuneSuffixes.end(), [&suffix](char const *known) {
        return suffix.compare(QLatin1StringView(known), Qt::CaseInsensitive) == 0;
    });
}

// mpv_set_option_string() returns an error code that is easy to drop on the floor, which hides typos and
// options a given libmpv does not have.
bool setMpvOption(mpv_handle *mpv, char const *name, char const *value) {
    const int rc = mpv_set_option_string(mpv, name, value);
    if(rc < 0) {
        qDebug() << "[mpv] option" << name << "=" << value << "rejected:" << mpv_error_string(rc);
        return false;
    }
    return true;
}

bool sendCommand(mpv_handle *mpv, QVariant const &command, QString const &name, uint64_t reply) {
    mpv::qt::node_builder node(command);
    const int rc = mpv_command_node_async(mpv, reply, node.node());
    if(rc < 0) {
        qDebug() << "[mpv] audio command" << name << "rejected:" << mpv_error_string(rc);
        return false;
    }
    return true;
}

Observed const *observed(uint64_t id) {
    for(Observed const &property : kObserved) {
        if(property.id == id)
            return &property;
    }
    return nullptr;
}

QVariant nodeValue(mpv_event_property const *property) {
    if(property->format != MPV_FORMAT_NODE)
        return QVariant();
    return mpv::qt::node_to_variant(static_cast<mpv_node *>(property->data));
}

QVariant stringValue(mpv_event_property const *property) {
    if(property->format != MPV_FORMAT_STRING)
        return QVariant();
    return QString::fromUtf8(*static_cast<char **>(property->data));
}

// Tag names keep whatever case the file uses ("TITLE" in Vorbis comments, "title" from ID3).
QVariantMap lowerCaseKeys(QVariantMap const &tags) {
    QVariantMap lowered;
    for(auto it = tags.cbegin(); it != tags.cend(); ++it) {
        const QString key = it.key().toLower();
        if(!lowered.contains(key))
            lowered.insert(key, it.value());
    }
    return lowered;
}

// True if `format` changed.
bool setFormatValue(QVariantMap &format, QString const &key, QVariant const &value) {
    if(!value.isValid() || value.toString().isEmpty())
        return format.remove(key) > 0;
    if(format.value(key) == value)
        return false;
    format.insert(key, value);
    return true;
}

} // namespace

AudioPlayerMpv::AudioPlayerMpv(QObject *parent) : AudioPlayer(parent) {
    mpv = mpv_create();
    if(!mpv) {
        qDebug() << "[mpv] could not create an mpv context for audio";
        return;
    }
    // Sound only: no window, no video decoding, and none of the command line player's habit of picking up
    // other files next to the one being played -- each of those would be one more file held open.
    setMpvOption(mpv, "vo", "null");
    setMpvOption(mpv, "vid", "no");
    setMpvOption(mpv, "audio-display", "no");
    setMpvOption(mpv, "cover-art-auto", "no");
    setMpvOption(mpv, "sub-auto", "no");
    setMpvOption(mpv, "audio-file-auto", "no");
    // At the end, hold the file paused instead of unloading it, so the seek bar still works there. That
    // pause, made once the audio has actually played out, is how the end is recognized; see onPause().
    setMpvOption(mpv, "keep-open", "yes");
    setMpvOption(mpv, "keep-open-pause", "yes");
    setMpvOption(mpv, "idle", "yes");
    // Mostly libmpv's defaults already. Spelled out because nothing from the user's own mpv setup (config,
    // scripts, saved positions, key bindings) belongs in qimgv, and ytdl is on by default.
    setMpvOption(mpv, "config", "no");
    setMpvOption(mpv, "load-scripts", "no");
    setMpvOption(mpv, "ytdl", "no");
    setMpvOption(mpv, "resume-playback", "no");
    setMpvOption(mpv, "input-default-bindings", "no");
    setMpvOption(mpv, "terminal", "no");
    // The name the stream gets in the system mixer (PulseAudio, PipeWire).
    setMpvOption(mpv, "audio-client-name", "qimgv");
    setMpvOption(mpv, "volume-max", "100");

    if(mpv_initialize(mpv) < 0) {
        qDebug() << "[mpv] could not initialize mpv for audio";
        mpv_terminate_destroy(mpv);
        mpv = nullptr;
        return;
    }
    for(Observed const &property : kObserved)
        mpv_observe_property(mpv, property.id, property.name, property.format);
    mpv_set_wakeup_callback(mpv, &AudioPlayerMpv::onWakeup, this);
}

AudioPlayerMpv::~AudioPlayerMpv() {
    if(!mpv)
        return;
    // mpv calls the wakeup callback from its own threads, and must not post to a half-destroyed object.
    mpv_set_wakeup_callback(mpv, nullptr, nullptr);
    mpv_terminate_destroy(mpv);
}

bool AudioPlayerMpv::open(QString const &file) {
    if(!mpv || file.isEmpty())
        return false;
    forget();
    // Named arguments: the position of "options" moved in mpv 0.38.
    QVariantMap load{{"name", "loadfile"}, {"url", file}};
    // For this file only, so that nothing else is taken on such slight evidence.
    if(isChiptune(file))
        load.insert("options", kChiptuneOptions);
    // Asynchronous, like everything here but stop(): mpv answers on its core thread, which can be busy for
    // a while -- opening an audio device takes up to a second with some Bluetooth headphones.
    if(!commandAsync(load, replyTag(kLoadFile)))
        return false;
    // Pause is not per file, and keep-open leaves it on at the end of the previous one. Sent after
    // "loadfile": before it, mpv would pause again at that end.
    setPauseAsync(false);
    return true;
}

void AudioPlayerMpv::stop() {
    if(!mpv)
        return;
    forget();
    // Callers rename, move or delete the file next, which Windows refuses (or leaves pending) while mpv still
    // has it open, depending on the libmpv and the filesystem. mpv closes it on its core thread some time
    // after "stop" has been carried out, so wait for that: once the command is done no file can start, and
    // every START_FILE so far is already queued, so all files are closed when each has had its END_FILE.
    // Asynchronous so that the timeout also covers a core that is busy elsewhere.
    if(!commandAsync({"stop"}, replyTag(kStop)))
        return;
    mStopPending = true;
    QElapsedTimer timer;
    timer.start();
    while(mStopPending || mOpenFiles > 0) {
        const qint64 left = kStopTimeoutMs - timer.elapsed();
        if(left <= 0) {
            qDebug() << "[mpv] audio file still open" << kStopTimeoutMs << "ms after stop";
            break;
        }
        handleEvent(mpv_wait_event(mpv, static_cast<double>(left) / 1000.0));
    }
}

void AudioPlayerMpv::unload() {
    if(!mpv)
        return;
    forget();
    // Nothing waits for it: what mpv still has to say about the file is dropped by generation, and a stop()
    // after this still waits for this file to close too, as it counts every START_FILE.
    commandAsync({"stop"}, kUntracked);
}

void AudioPlayerMpv::setPaused(bool paused) {
    if(!mpv)
        return;
    if(!paused && mAtEnd) {
        restart(true);
        return;
    }
    // A seek still waiting would otherwise land after the unpause: at the end, mpv would pause there again.
    if(mSeek.pending)
        sendSeek();
    setPauseAsync(paused);
}

bool AudioPlayerMpv::isPaused() const {
    return mPaused;
}

void AudioPlayerMpv::seek(double seconds) {
    if(mEntry != Entry::Loaded || !std::isfinite(seconds))
        return;
    // mpv counts a negative absolute time from the end.
    seconds = std::max(0.0, seconds);
    seekingTo(seconds);
    queueSeek(seconds, false);
}

void AudioPlayerMpv::seekRelative(double seconds) {
    if(mEntry != Entry::Loaded || !std::isfinite(seconds))
        return;
    seekingTo(mPosition + seconds);
    queueSeek(seconds, true);
}

void AudioPlayerMpv::setVolume(int volume) {
    volume = qBound(0, volume, 100);
    if(!mpv || volume == mVolume)
        return;
    mVolume = volume;
    mVolumeSet.pending = true;
    if(mVolumeSet.inFlight == 0)
        sendVolume();
}

void AudioPlayerMpv::setMuted(bool muted) {
    if(!mpv)
        return;
    mMuted = muted;
    mMuteSet.pending = true;
    if(mMuteSet.inFlight == 0)
        sendMute();
}

void AudioPlayerMpv::setLoop(bool loop) {
    if(!mpv)
        return;
    mLoop = loop;
    char const *value = loop ? "inf" : "no";
    setAsync("loop-file", MPV_FORMAT_STRING, &value);
}

// Called on one of mpv's threads.
void AudioPlayerMpv::onWakeup(void *ctx) {
    auto self = static_cast<AudioPlayerMpv *>(ctx);
    if(!self->mDrainQueued.exchange(true))
        QMetaObject::invokeMethod(self, [self] { self->drainEvents(); }, Qt::QueuedConnection);
}

void AudioPlayerMpv::drainEvents() {
    mDrainQueued = false;
    if(!mpv)
        return;
    while(true) {
        mpv_event *event = mpv_wait_event(mpv, 0);
        if(event->event_id == MPV_EVENT_NONE)
            break;
        handleEvent(event);
    }
    // The format is made of several properties, which tend to arrive together.
    flushFormat();
}

// Nothing here may touch `event` after emitting a signal: a slot may call stop(), which reads further events
// and with that reuses the memory mpv handed this one out in.
void AudioPlayerMpv::handleEvent(mpv_event *event) {
    switch(event->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE:
        // mpv discards what it has read of every property when a file starts, loads or ends, so whatever
        // arrives once our FILE_LOADED is in was read after it, and is about our file.
        if(mEntry == Entry::Loaded)
            onPropertyChange(event->reply_userdata, static_cast<mpv_event_property *>(event->data));
        break;
    case MPV_EVENT_START_FILE: {
        // No payload before libmpv 0.33.
        auto startFile = static_cast<mpv_event_start_file *>(event->data);
        onStartFile(startFile ? startFile->playlist_entry_id : kAnyEntry);
        break;
    }
    case MPV_EVENT_FILE_LOADED:
        onFileLoaded();
        break;
    case MPV_EVENT_END_FILE:
        onEndFile(static_cast<mpv_event_end_file *>(event->data));
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
        if(mEntry != Entry::Loaded)
            break;
        // The decoder has produced audio, so its output format is known now. When it is the same as the
        // previous file's, mpv reports neither a change of audio-params nor an audio reconfig.
        if(!mFormat.contains("samplerate"))
            read(kAudioParams);
        mBitrateFrom.reset();
        // Paused, so keep-open has nothing to pause should this seek have reached the end; see seekingTo().
        // When the target did not say, eof-reached is asked, which is usually -- not always -- set by now.
        if(mPauseRequested && mSeekToEnd)
            reachedEnd();
        else if(mPauseRequested)
            read(kEofReached);
        break;
    case MPV_EVENT_COMMAND_REPLY:
        onCommandReply(event);
        break;
    case MPV_EVENT_GET_PROPERTY_REPLY:
        onGetReply(event);
        break;
    case MPV_EVENT_SET_PROPERTY_REPLY:
        if(event->error < 0)
            qDebug() << "[mpv] audio property rejected:" << mpv_error_string(event->error);
        switch(event->reply_userdata & kKindMask) {
        case kPauseSet:
            mPausesInFlight = std::max(0, mPausesInFlight - 1);
            break;
        case kVolumeSet:
            mVolumeSet.inFlight = std::max(0, mVolumeSet.inFlight - 1);
            if(mVolumeSet.pending && mVolumeSet.inFlight == 0)
                sendVolume();
            break;
        case kMuteSet:
            mMuteSet.inFlight = std::max(0, mMuteSet.inFlight - 1);
            if(mMuteSet.pending && mMuteSet.inFlight == 0)
                sendMute();
            break;
        default:;
        }
        break;
    case MPV_EVENT_QUEUE_OVERFLOW:
        // START_FILE / END_FILE pairs may be among what was lost; stop() must not wait for an END_FILE that
        // will never come.
        qDebug() << "[mpv] audio event queue overflowed";
        mOpenFiles = 0;
        break;
    default:;
    }
}

void AudioPlayerMpv::onCommandReply(mpv_event const *event) {
    if(event->error < 0)
        qDebug() << "[mpv] audio command failed:" << mpv_error_string(event->error);
    const uint64_t kind = event->reply_userdata & kKindMask;
    // Whichever file it was for: forget() drops a seek still waiting, not the count of those in flight.
    if(kind == kSeek) {
        mSeek.inFlight = std::max(0, mSeek.inFlight - 1);
        if(mSeek.pending && mSeek.inFlight == 0)
            sendSeek();
        return;
    }
    if(kind == kUntracked || (event->reply_userdata >> kKindBits) != mGeneration)
        return;
    if(kind == kStop) {
        mStopPending = false;
        return;
    }
    if(kind != kLoadFile || mEntry != Entry::None)
        return;
    if(event->error < 0) {
        mEntry = Entry::Ended;
        emit failed(QString::fromUtf8(mpv_error_string(event->error)));
        return;
    }
    auto command = static_cast<mpv_event_command *>(event->data);
    const QVariant id = mpv::qt::node_to_variant(&command->result).toMap().value("playlist_entry_id");
    mEntryId = id.isValid() ? id.toLongLong() : kAnyEntry;
    mEntry = Entry::Queued;
}

void AudioPlayerMpv::onGetReply(mpv_event const *event) {
    const uint64_t kind = event->reply_userdata & kKindMask;
    if((event->reply_userdata >> kKindBits) != mGeneration || mEntry != Entry::Loaded)
        return;
    // Unavailable reads the same as an observation of an unavailable property.
    Observed const *property = observed(kind);
    mpv_event_property unavailable{property ? property->name : "", MPV_FORMAT_NONE, nullptr};
    auto data = event->error < 0 ? &unavailable : static_cast<mpv_event_property const *>(event->data);
    if(kind != kBitrate && kind != kDemuxBitrate) {
        onPropertyChange(kind, data);
        return;
    }
    const double bitrate = nodeValue(data).toDouble();
    // Too little was read to measure it: a file shorter than kBitrateAfter, or a seek just before. The
    // demuxer's figure comes from the stream headers, where there is one.
    if(bitrate <= 0 && kind == kBitrate) {
        read(kDemuxBitrate);
        return;
    }
    mBitrate = Bitrate::Known;
    mFormatDirty |= setFormatValue(mFormat, "bitrate", qlonglong(std::llround(std::max(0.0, bitrate))));
}

void AudioPlayerMpv::onStartFile(int64_t entryId) {
    ++mOpenFiles;
    if(mEntry == Entry::Queued && isOurs(entryId))
        mEntry = Entry::Started;
}

void AudioPlayerMpv::onFileLoaded() {
    if(mEntry != Entry::Started)
        return;
    mEntry = Entry::Loaded;
    // mpv only reports a property whose value differs from the last one it reported, and the previous file
    // may well have had the same duration, tags or codec. So these are read, not waited for.
    for(uint64_t property : {kDuration, kMetadata, kCodecName, kCodecDescription, kFileFormat})
        read(property);
    const uint64_t generation = mGeneration;
    emit fileLoaded();
    if(generation == mGeneration)
        syncPaused();
}

void AudioPlayerMpv::onEndFile(mpv_event_end_file const *endFile) {
    mOpenFiles = std::max(0, mOpenFiles - 1);
    if((mEntry != Entry::Started && mEntry != Entry::Loaded) || !isOurs(endFile->playlist_entry_id))
        return;
    mEntry = Entry::Ended;
    switch(endFile->reason) {
    case MPV_END_FILE_REASON_ERROR:
        emit failed(QString::fromUtf8(mpv_error_string(endFile->error)));
        break;
    case MPV_END_FILE_REASON_REDIRECT:
        // A playlist, whatever its name says, and mpv would go on to play the files it lists.
        commandAsync({"stop"}, kUntracked);
        emit failed(QString::fromUtf8(mpv_error_string(MPV_ERROR_UNKNOWN_FORMAT)));
        break;
    case MPV_END_FILE_REASON_EOF:
        // keep-open should have held the file at its end; ended is ended all the same.
        if(!mAtEnd) {
            mAtEnd = true;
            emit playbackFinished();
        }
        break;
    default:;
    }
}

void AudioPlayerMpv::onPropertyChange(uint64_t property, mpv_event_property const *data) {
    switch(property) {
    case kTimePos:
        if(data->format == MPV_FORMAT_DOUBLE) {
            mPosition = *static_cast<double *>(data->data);
            if(!mBitrateFrom)
                mBitrateFrom = mPosition;
            else if(mBitrate == Bitrate::Wanted && mPosition - *mBitrateFrom >= kBitrateAfter)
                read(kBitrate);
            emit positionChanged(mPosition);
        }
        break;
    case kDuration: {
        const double duration = data->format == MPV_FORMAT_DOUBLE ? *static_cast<double *>(data->data) : 0;
        if(duration != mDuration) {
            mDuration = duration;
            emit durationChanged(duration);
        }
        break;
    }
    case kPause:
        if(data->format == MPV_FORMAT_FLAG)
            onPause(*static_cast<int *>(data->data) != 0);
        break;
    case kEofReached:
        onEofReached(data->format == MPV_FORMAT_FLAG && *static_cast<int *>(data->data) != 0);
        break;
    case kMetadata: {
        if(data->format != MPV_FORMAT_NODE)
            break;
        QVariantMap tags = lowerCaseKeys(nodeValue(data).toMap());
        if(tags != mMetadata) {
            mMetadata = tags;
            emit metadataChanged(tags);
        }
        break;
    }
    case kAudioParams: {
        const QVariantMap params = nodeValue(data).toMap();
        const QVariant layout = params.value("hr-channels");
        mFormatDirty |= setFormatValue(mFormat, "samplerate", params.value("samplerate"));
        mFormatDirty |= setFormatValue(mFormat, "channels", params.value("channel-count"));
        mFormatDirty |=
            setFormatValue(mFormat, "channel-layout", layout.toString().isEmpty() ? params.value("channels") : layout);
        mFormatDirty |= setFormatValue(mFormat, "sample-format", params.value("format"));
        break;
    }
    case kCodecName:
        mFormatDirty |= setFormatValue(mFormat, "codec", stringValue(data));
        break;
    case kCodecDescription:
        mFormatDirty |= setFormatValue(mFormat, "codec-description", stringValue(data));
        break;
    case kFileFormat:
        mFormatDirty |= setFormatValue(mFormat, "container", stringValue(data));
        break;
    default:;
    }
}

void AudioPlayerMpv::onPause(bool paused) {
    mPaused = paused;
    // Not one of ours that mpv has yet to catch up with, so keep-open's.
    const bool own = !mPauseRequested && mPausesInFlight == 0;
    const uint64_t generation = mGeneration;
    syncPaused();
    // eof-reached is true while the last of the audio still sits in the output buffer, and keep-open pauses
    // once that has been heard. A pause asked for in that last moment ends up there too: mpv plays the
    // buffer out first, and will not unpause at the end.
    if(paused && (own || mEofReached) && generation == mGeneration)
        reachedEnd();
}

void AudioPlayerMpv::onEofReached(bool eof) {
    mEofReached = eof;
    if(!eof) {
        mAtEnd = false;
        return;
    }
    // A seek to the end while paused, where keep-open has nothing to pause.
    if(mPaused)
        reachedEnd();
}

void AudioPlayerMpv::reachedEnd() {
    if(mBitrate == Bitrate::Wanted)
        read(kBitrate);
    // mpv loops by itself without stopping at the end. It got here because setLoop(true) came too late for
    // it to see, and keep-open paused it, which is no reason to stay paused -- or because a seek took a
    // track the user had paused to its end, which is.
    if(mLoop) {
        restart(!mPauseRequested);
        return;
    }
    if(mAtEnd)
        return;
    mAtEnd = true;
    emit playbackFinished();
}

void AudioPlayerMpv::flushFormat() {
    if(!mFormatDirty || mEntry != Entry::Loaded)
        return;
    mFormatDirty = false;
    emit formatChanged(mFormat);
}

// The first pausedChanged() for a file comes with FILE_LOADED: mpv only reports changes, and pause carries
// over from the previous file.
void AudioPlayerMpv::syncPaused() {
    if(mEntry != Entry::Loaded || mPausedSent == mPaused)
        return;
    mPausedSent = mPaused;
    emit pausedChanged(mPaused);
}

void AudioPlayerMpv::read(uint64_t property) {
    char const *name = nullptr;
    mpv_format format = MPV_FORMAT_NODE;
    if(property == kBitrate) {
        name = "audio-bitrate";
    } else if(property == kDemuxBitrate) {
        name = "current-tracks/audio/demux-bitrate";
    } else if(Observed const *o = observed(property)) {
        name = o->name;
        format = o->format;
    } else {
        return;
    }
    if(property == kBitrate || property == kDemuxBitrate)
        mBitrate = Bitrate::Requested;
    const int rc = mpv_get_property_async(mpv, replyTag(property), name, format);
    if(rc < 0) {
        qDebug() << "[mpv] could not read" << name << ":" << mpv_error_string(rc);
        if(mBitrate == Bitrate::Requested)
            mBitrate = Bitrate::Known;
    }
}

// keep-open holds mpv paused at the end, where unpausing would only end the file again.
void AudioPlayerMpv::restart(bool resume) {
    mSeekTarget = 0;
    mSeekRelative = false;
    sendSeek();
    mSeekToEnd = false;
    mEofReached = false;
    mAtEnd = false;
    if(resume)
        setPauseAsync(false);
}

// Where a seek is headed decides whether it leaves the end or reaches it, not what mpv reports after: a
// setPaused() may come before mpv has caught up, and while paused mpv does not report arriving at the end at
// all (eof-reached turns true just after PLAYBACK_RESTART, and is not announced while its core is idle).
// With no duration to go by, a seek is taken to leave the end; mpv says so again if it did not.
void AudioPlayerMpv::seekingTo(double target) {
    mSeekToEnd = mDuration > 0 && target >= mDuration;
    if(!mSeekToEnd) {
        mEofReached = false;
        mAtEnd = false;
    }
}

void AudioPlayerMpv::queueSeek(double seconds, bool relative) {
    if(mSeek.pending && relative) {
        mSeekTarget = mSeekRelative ? mSeekTarget + seconds : std::max(0.0, mSeekTarget + seconds);
    } else {
        mSeekTarget = seconds;
        mSeekRelative = relative;
    }
    mSeek.pending = true;
    if(mSeek.inFlight == 0)
        sendSeek();
}

void AudioPlayerMpv::sendSeek() {
    mSeek.pending = false;
    if(commandAsync({"seek", mSeekTarget, mSeekRelative ? "relative+exact" : "absolute+exact"}, kSeek))
        ++mSeek.inFlight;
}

void AudioPlayerMpv::sendVolume() {
    mVolumeSet.pending = false;
    double value = mVolume;
    if(setAsync("volume", MPV_FORMAT_DOUBLE, &value, kVolumeSet))
        ++mVolumeSet.inFlight;
}

void AudioPlayerMpv::sendMute() {
    mMuteSet.pending = false;
    int flag = mMuted;
    if(setAsync("mute", MPV_FORMAT_FLAG, &flag, kMuteSet))
        ++mMuteSet.inFlight;
}

// Whatever mpv still has to say about the previous file is dropped from here on: events are only let
// through for the entry a "loadfile" reply of the current generation named.
void AudioPlayerMpv::forget() {
    ++mGeneration;
    mEntry = Entry::None;
    mEntryId = kNoEntry;
    mPausedSent.reset();
    mDuration = 0;
    mPosition = 0;
    mEofReached = false;
    mAtEnd = false;
    mSeekToEnd = false;
    mMetadata.clear();
    mFormat.clear();
    mFormatDirty = false;
    mBitrate = Bitrate::Wanted;
    mBitrateFrom.reset();
    mSeek.pending = false;
}

bool AudioPlayerMpv::isOurs(int64_t entryId) const {
    return mEntryId == kAnyEntry || entryId == mEntryId;
}

uint64_t AudioPlayerMpv::replyTag(uint64_t kind) const {
    return (mGeneration << kKindBits) | kind;
}

bool AudioPlayerMpv::setAsync(char const *name, mpv_format format, void *value, uint64_t reply) {
    const int rc = mpv_set_property_async(mpv, reply, name, format, value);
    if(rc < 0) {
        qDebug() << "[mpv] audio property" << name << "rejected:" << mpv_error_string(rc);
        return false;
    }
    return true;
}

void AudioPlayerMpv::setPauseAsync(bool paused) {
    mPaused = paused;
    mPauseRequested = paused;
    int flag = paused;
    if(setAsync("pause", MPV_FORMAT_FLAG, &flag, kPauseSet))
        ++mPausesInFlight;
}

bool AudioPlayerMpv::commandAsync(QVariantList const &args, uint64_t reply) {
    return sendCommand(mpv, args, args.value(0).toString(), reply);
}

bool AudioPlayerMpv::commandAsync(QVariantMap const &args, uint64_t reply) {
    return sendCommand(mpv, args, args.value("name").toString(), reply);
}

AudioPlayer *CreateAudioPlayer() {
    return new AudioPlayerMpv();
}
