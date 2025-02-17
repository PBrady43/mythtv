// -*- Mode: c++ -*-
/** TextSubtitles
 *  Copyright (c) 2006 by Pekka Jääskeläinen
 *  Distributed as part of MythTV under GPL v2 and later.
 */

// ANSI C
#include <cstdio>
#include <cstring>
#include <climits>

// C++
#include <algorithm>

// Qt
#include <QRunnable>
#include <QTextCodec>
#include <QFile>
#include <QDataStream>

// MythTV
#include "mythcorecontext.h"
#include "remotefile.h"
#include "captions/textsubtitleparser.h"
#include "captions/xine_demux_sputext.h"
#include "mythlogging.h"
#include "mthreadpool.h"

// This background thread helper class is adapted from the
// RebuildSaver class in mythcommflagplayer.cpp.
class SubtitleLoadHelper : public QRunnable
{
  public:
    SubtitleLoadHelper(const QString &fileName,
                       TextSubtitles *target)
        : m_fileName(fileName), m_target(target)
    {
        QMutexLocker locker(&s_lock);
        ++s_loading[m_target];
    }

    void run(void) override // QRunnable
    {
        TextSubtitleParser::LoadSubtitles(m_fileName, *m_target, false);

        QMutexLocker locker(&s_lock);
        --s_loading[m_target];
        if (!s_loading[m_target])
            s_wait.wakeAll();
    }

    static bool IsLoading(TextSubtitles *target)
    {
        QMutexLocker locker(&s_lock);
        return s_loading[target] != 0U;
    }

    static void Wait(TextSubtitles *target)
    {
        QMutexLocker locker(&s_lock);
        if (!s_loading[target])
            return;
        while (s_wait.wait(&s_lock))
        {
            if (!s_loading[target])
                return;
        }
    }

  private:
    const QString &m_fileName;
    TextSubtitles *m_target;

    static QMutex                     s_lock;
    static QWaitCondition             s_wait;
    static QHash<TextSubtitles*,uint> s_loading;
};
QMutex                     SubtitleLoadHelper::s_lock;
QWaitCondition             SubtitleLoadHelper::s_wait;
QHash<TextSubtitles*,uint> SubtitleLoadHelper::s_loading;

// Work around the fact that RemoteFile doesn't work when the target
// file is actually local.
class RemoteFileWrapper
{
public:
    explicit RemoteFileWrapper(const QString &filename) {
        // This test stolen from FileRingBuffer::OpenFile()
        bool is_local =
            (!filename.startsWith("/dev")) &&
            ((filename.startsWith("/")) || QFile::exists(filename));
        m_isRemote = !is_local;
        if (m_isRemote)
        {
            m_localFile = nullptr;
            m_remoteFile = new RemoteFile(filename, false, false, 0s);
        }
        else
        {
            m_remoteFile = nullptr;
            m_localFile = new QFile(filename);
            if (!m_localFile->open(QIODevice::ReadOnly))
            {
                delete m_localFile;
                m_localFile = nullptr;
            }
        }
    }
    ~RemoteFileWrapper() {
        delete m_remoteFile;
        delete m_localFile;
    }

    RemoteFileWrapper(const RemoteFileWrapper &) = delete;            // not copyable
    RemoteFileWrapper &operator=(const RemoteFileWrapper &) = delete; // not copyable

    bool isOpen(void) const {
        if (m_isRemote)
            return m_remoteFile->isOpen();
        return m_localFile;
    }
    long long GetFileSize(void) const {
        if (m_isRemote)
            return m_remoteFile->GetFileSize();
        if (m_localFile)
            return m_localFile->size();
        return 0;
    }
    int Read(void *data, int size) {
        if (m_isRemote)
            return m_remoteFile->Read(data, size);
        if (m_localFile)
        {
            QDataStream stream(m_localFile);
            return stream.readRawData(static_cast<char*>(data), size);
        }
        return 0;
    }
private:
    bool m_isRemote;
    RemoteFile *m_remoteFile;
    QFile *m_localFile;
};

static bool operator<(const text_subtitle_t& left,
                      const text_subtitle_t& right)
{
    return left.m_start < right.m_start;
}

TextSubtitles::~TextSubtitles()
{
    SubtitleLoadHelper::Wait(this);
}

/** \fn TextSubtitles::HasSubtitleChanged(uint64_t timecode) const
 *  \brief Returns true in case the subtitle to display has changed
 *         since the last GetSubtitles() call.
 *
 *  This is used to avoid redisplaying subtitles that are already displaying.
 *
 *  \param timecode The timecode (frame number or time stamp)
 *         of the current video position.
 *  \return True in case new subtitles should be displayed.
 */
bool TextSubtitles::HasSubtitleChanged(uint64_t timecode) const
{
    return (timecode < m_lastReturnedSubtitle.m_start ||
            timecode > m_lastReturnedSubtitle.m_end);
}

/** \fn TextSubtitles::GetSubtitles(uint64_t timecode) const
 *  \brief Returns the subtitles to display at the given timecode.
 *
 *  \param timecode The timecode (frame number or time stamp) of the
 *         current video position.
 *  \return The subtitles as a list of strings.
 */
QStringList TextSubtitles::GetSubtitles(uint64_t timecode)
{
    QStringList list;
    if (!m_isInProgress && m_subtitles.empty())
        return list;

    text_subtitle_t searchTarget(timecode, timecode);

    auto nextSubPos =
        std::lower_bound(m_subtitles.begin(), m_subtitles.end(), searchTarget);

    uint64_t startCode = 0;
    uint64_t endCode = 0;
    if (nextSubPos != m_subtitles.begin())
    {
        auto currentSubPos = nextSubPos;
        --currentSubPos;

        const text_subtitle_t &sub = *currentSubPos;
        if (sub.m_start <= timecode && sub.m_end >= timecode)
        {
            // found a sub to display
            m_lastReturnedSubtitle = sub;
            return m_lastReturnedSubtitle.m_textLines;
        }

        // the subtitle time span has ended, let's display a blank sub
        startCode = sub.m_end + 1;
    }

    if (nextSubPos == m_subtitles.end())
    {
        if (m_isInProgress)
        {
            const int maxReloadInterval = 1000; // ms
            if (IsFrameBasedTiming())
            {
                // Assume conservative 24fps
                endCode = startCode + maxReloadInterval / 24;
            }
            else
            {
                endCode = startCode + maxReloadInterval;
            }
            QDateTime now = QDateTime::currentDateTimeUtc();
            if (!m_fileName.isEmpty() &&
                m_lastLoaded.msecsTo(now) >= maxReloadInterval)
            {
                TextSubtitleParser::LoadSubtitles(m_fileName, *this, true);
            }
        }
        else
        {
            // at the end of video, the blank subtitle should last
            // until forever
            endCode = startCode + INT_MAX;
        }
    }
    else
    {
        endCode = (*nextSubPos).m_start - 1;
    }

    // we are in a position in which there are no subtitles to display,
    // return an empty subtitle and create a dummy empty subtitle for this
    // time span so SubtitleChanged() functions also in this case
    text_subtitle_t blankSub(startCode, endCode);
    m_lastReturnedSubtitle = blankSub;

    return list;
}

void TextSubtitles::AddSubtitle(const text_subtitle_t &newSub)
{
    QMutexLocker locker(&m_lock);
    m_subtitles.push_back(newSub);
}

void TextSubtitles::Clear(void)
{
    QMutexLocker locker(&m_lock);
    m_subtitles.clear();
}

void TextSubtitles::SetLastLoaded(void)
{
    emit TextSubtitlesUpdated();
    QMutexLocker locker(&m_lock);
    m_lastLoaded = QDateTime::currentDateTimeUtc();
}

void TextSubtitleParser::LoadSubtitles(const QString &fileName,
                                       TextSubtitles &target,
                                       bool inBackground)
{
    if (inBackground)
    {
        if (!SubtitleLoadHelper::IsLoading(&target))
        {
            MThreadPool::globalInstance()->
                start(new SubtitleLoadHelper(fileName, &target),
                      "SubtitleLoadHelper");
        }
        return;
    }
    demux_sputext_t sub_data {};
    RemoteFileWrapper rfile(fileName/*, false, false, 0*/);

    LOG(VB_VBI, LOG_INFO,
        QString("Preparing to load subtitle file (%1)").arg(fileName));
    if (!rfile.isOpen())
    {
        LOG(VB_VBI, LOG_INFO,
            QString("Failed to load subtitle file (%1)").arg(fileName));
        return;
    }
    target.SetHasSubtitles(true);
    target.SetFilename(fileName);

    // Only reload if rfile.GetFileSize() has changed.
    off_t new_len = rfile.GetFileSize();
    if (target.GetByteCount() == new_len)
    {
        LOG(VB_VBI, LOG_INFO,
            QString("Filesize unchanged (%1), not reloading subs (%2)")
            .arg(new_len).arg(fileName));
        target.SetLastLoaded();
        return;
    }
    LOG(VB_VBI, LOG_INFO,
        QString("Preparing to read %1 subtitle bytes from %2")
        .arg(new_len).arg(fileName));
    target.SetByteCount(new_len);
    sub_data.rbuffer_len = new_len;
    sub_data.rbuffer_text = new char[sub_data.rbuffer_len + 1];
    sub_data.rbuffer_cur = 0;
    sub_data.errs = 0;
    int numread = rfile.Read(sub_data.rbuffer_text, sub_data.rbuffer_len);
    LOG(VB_VBI, LOG_INFO,
        QString("Finished reading %1 subtitle bytes (requested %2)")
        .arg(numread).arg(new_len));

    // try to determine the text codec
    QByteArray test(sub_data.rbuffer_text, sub_data.rbuffer_len);
    QTextCodec *textCodec = QTextCodec::codecForUtfText(test, nullptr);
    if (!textCodec)
    {
        LOG(VB_VBI, LOG_WARNING, "Failed to autodetect a UTF encoding.");
        QString codec = gCoreContext->GetSetting("SubtitleCodec", "");
        if (!codec.isEmpty())
            textCodec = QTextCodec::codecForName(codec.toLatin1());
        if (!textCodec)
            textCodec = QTextCodec::codecForName("utf-8");
        if (!textCodec)
        {
            LOG(VB_VBI, LOG_ERR,
                QString("Failed to find codec for subtitle file '%1'")
                .arg(fileName));
            return;
        }
    }

    LOG(VB_VBI, LOG_INFO, QString("Opened subtitle file '%1' with codec '%2'")
        .arg(fileName, textCodec->name().constData()));

    // load the entire subtitle file, converting to unicode as we go
    QScopedPointer<QTextDecoder> dec(textCodec->makeDecoder());
    QString data = dec->toUnicode(sub_data.rbuffer_text, sub_data.rbuffer_len);
    if (data.isEmpty())
    {
        LOG(VB_VBI, LOG_WARNING,
            QString("Data loaded from subtitle file '%1' is empty.")
            .arg(fileName));
        return;
    }

    // convert back to utf-8 for parsing
    QByteArray ba = data.toUtf8();
    delete[] sub_data.rbuffer_text;
    sub_data.rbuffer_text = ba.data();
    sub_data.rbuffer_len = ba.size();

    try
    {
        if (!sub_read_file(&sub_data))
        {
            // Don't delete[] sub_data.rbuffer_text; because the
            // QByteArray destructor will clean up.
            LOG(VB_VBI, LOG_ERR, QString("Failed to read subtitles from '%1'")
                .arg(fileName));
            return;
        }
    } catch (std::exception& e) {
        LOG(VB_VBI, LOG_ERR,
            QString("Exception reading subtitles file (%1)").arg(e.what()));
        return;
    }

    LOG(VB_VBI, LOG_INFO, QString("Found %1 subtitles in file '%2'")
        .arg(sub_data.num).arg(fileName));
    target.SetFrameBasedTiming(sub_data.uses_time == 0);
    target.Clear();

    // convert the subtitles to our own format, free the original structures
    // and convert back to unicode
    textCodec = QTextCodec::codecForName("utf-8");
    if (textCodec)
        dec.reset(textCodec->makeDecoder());

    for (const auto& sub : sub_data.subtitles)
    {
        text_subtitle_t newsub(sub.start, sub.end);

        if (!target.IsFrameBasedTiming())
        {
            newsub.m_start *= 10; // convert from csec to msec
            newsub.m_end *= 10;
        }

        for (const auto & line : sub.text)
        {
            const char *subLine = line.c_str();
            QString str;
            if (textCodec)
                str = dec->toUnicode(subLine, strlen(subLine));
            else
                str = QString(subLine);
            newsub.m_textLines.push_back(str);
        }
        target.AddSubtitle(newsub);
    }

    // textCodec object is managed by Qt, do not delete...

    // Don't delete[] sub_data.rbuffer_text; because the QByteArray
    // destructor will clean up.

    LOG(VB_GENERAL, LOG_INFO, QString("Loaded %1 subtitles from '%2'")
        .arg(target.GetSubtitleCount()).arg(fileName));
    target.SetLastLoaded();
}
