/* BEGIN_COMMON_COPYRIGHT_HEADER
 * (c)LGPL2+
 *
 * Flacon - audio File Encoder
 * https://github.com/flacon/flacon
 *
 * Copyright: 2012-2013
 *   Alexander Sokoloff <sokoloff.a@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * END_COMMON_COPYRIGHT_HEADER */


#include "trackviewmodel.h"
#include "trackview.h"
#include "project.h"
#include "disk.h"
#include "settings.h"

#include <QDebug>
#include <QSet>

struct CacheTrackData
{
    CacheTrackData():
        state(TrackState::NotRunning),
        percent(0),
        diskNum(-1),
        trackNum(-1)
    {
    }

    TrackState state;
    Percent percent;
    DiskNum diskNum;
    TrackNum trackNum;
};


class TrackViewModel::Cache
{
public:
   Cache()  {}
   QSet<DiskNum> downloadedDisks;
   QHash<Track, CacheTrackData> tracks;
};


class IndexData
{
public:
    explicit IndexData(const QModelIndex &index)
    {
        mDiskId  = (quint32(index.internalId()) & 0xFFFFFF);
        mTrackId = (quint32(index.internalId()) >> 16);
    }

    explicit IndexData(quint16 diskNum, quint16 trackNum)
    {
        mDiskId  = diskNum  + 1;
        mTrackId = trackNum + 1;
    }

    explicit IndexData(quint16 diskNum)
    {
        mDiskId  = diskNum + 1;
        mTrackId = 0;
    }


    quintptr asPtr()
    {
        return quintptr((mTrackId << 16) |  mDiskId);
    }


    bool isDisk()  const { return mDiskId  > 0; }
    bool isTrack() const { return mTrackId > 0; }

    int diskNum()  const { return mDiskId  - 1; }
    int trackNum() const { return mTrackId - 1; }

    Disk *disk() const
    {
        if (mDiskId && mDiskId-1 < project->count())
            return project->disk(mDiskId - 1);

        return NULL;
    }

    Track *track() const
    {
        if (mTrackId)
        {
            Disk *disk = this->disk();
            if (disk && mTrackId -1 < disk->count())
                return disk->track(mTrackId - 1);
        }
        return NULL;
    }

private:
    quint16 mDiskId;
    quint16 mTrackId;
};


/************************************************
 *
 ************************************************/
inline uint qHash(const Track &track, uint seed) {
    return qHash(QPair<int, QString>(track.trackNum(), track.cueFileName()), seed);
}


/************************************************

 ************************************************/
TrackViewModel::TrackViewModel(TrackView *parent) :
    QAbstractItemModel(parent),
    mCache(new Cache()),
    mView(parent)
{
    connect(project, &Project::diskChanged,
            this, &TrackViewModel::diskDataChanged);

    connect(project, SIGNAL(layoutChanged()), this, SIGNAL(layoutChanged()));
    connect(project, SIGNAL(afterRemoveDisk()), this, SIGNAL(layoutChanged()));

    connect(project, &Project::beforeRemoveDisk,
            this, &TrackViewModel::invalidateCache);
}


/************************************************
 *
 ************************************************/
TrackViewModel::~TrackViewModel()
{
    delete mCache;
}


/************************************************

 ************************************************/
QVariant TrackViewModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    switch(section)
    {
    case TrackView::ColumnTracknum:   return QVariant(tr("Track",   "Table header."));
    case TrackView::ColumnDuration:   return QVariant(tr("Length",  "Table header."));
    case TrackView::ColumnTitle:      return QVariant(tr("Title",   "Table header."));
    case TrackView::ColumnArtist:     return QVariant(tr("Artist",  "Table header."));
    case TrackView::ColumnAlbum:      return QVariant(tr("Album",   "Table header."));
    case TrackView::ColumnComment:    return QVariant(tr("Comment", "Table header."));
    case TrackView::ColumnFileName:   return QVariant(tr("File",    "Table header."));
    }
    return QVariant();
}


/************************************************

 ************************************************/
QModelIndex TrackViewModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return QModelIndex();


    if (IndexData(parent).isTrack())
        return QModelIndex();

    if (IndexData(parent).isDisk())
        return createIndex(row, column, IndexData(parent.row(), row).asPtr());

    return createIndex(row, column, IndexData(row).asPtr());
}


/************************************************

 ************************************************/
QModelIndex TrackViewModel::index(const Disk &disk, int col) const
{
    const int diskNum = project->indexOf(&disk);
    if (diskNum > -1 && diskNum < rowCount(QModelIndex()))
        return index(diskNum, col, QModelIndex());

    return QModelIndex();
}


/************************************************
 *
 ************************************************/
QModelIndex TrackViewModel::index(const Track &track, int col) const
{
    CacheTrackData &cache = mCache->tracks[track];

    const IndexData indexData(cache.diskNum, cache.trackNum);
    if (indexData.track() && *indexData.track() == track)
    {
        return index(cache.trackNum, col, index(cache.diskNum, 0));
    }

    // Update cache
    for (int d=0; d<rowCount(); ++d)
    {
        QModelIndex diskIndex = index(d, 0);
        for (int t=0; t<rowCount(diskIndex); ++t)
        {
            IndexData indexData(d, t);
            if (indexData.track() && *indexData.track() == track)
            {
                cache.diskNum  = d;
                cache.trackNum = t;
                return index(cache.trackNum, col, diskIndex);
            }
        }
    }

    return QModelIndex();
}


/************************************************

 ************************************************/
QModelIndex TrackViewModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return QModelIndex();

    IndexData data(child);
    if (data.isTrack())
        return index(data.diskNum(), 0, QModelIndex());

    return QModelIndex();
}


/************************************************

 ************************************************/
QVariant TrackViewModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    IndexData indexData(index);

    Track *track = indexData.track();
    if (track)
        return trackData(track, index, role);

    Disk *disk = indexData.disk();
    if(disk)
        return diskData(disk, index, role);

    return QVariant();
}


/************************************************

 ************************************************/
bool TrackViewModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid())
        return false;

    if (role != Qt::EditRole)
        return false;


    QList<Track*> tracks = view()->selectedTracks();
    foreach(Track *track, tracks)
    {
        switch (index.column())
        {
        case TrackView::ColumnTitle:
            track->setTitle(value.toString());
            break;

        case TrackView::ColumnArtist:
            track->setArtist(value.toString());
            break;

        case TrackView::ColumnAlbum:
            track->setAlbum(value.toString());
            break;

        case TrackView::ColumnComment:
            track->setComment(value.toString());
            break;
        }
    }

    emit dataChanged(index, index, QVector<int>() << role);
    return true;
}


/************************************************

 ************************************************/
QVariant TrackViewModel::trackData(const Track *track, const QModelIndex &index, int role) const
{
    // Display & Edit :::::::::::::::::::::::::::::::::::
    if (role == Qt::DisplayRole || role == Qt::EditRole )
    {
        switch (index.column())
        {
        case TrackView::ColumnTracknum:
            return QVariant(QString("%1").arg(track->trackNum(), 2, 10, QChar('0')));

        case TrackView::ColumnDuration:
            return QVariant(trackDurationToString(track->duration()) + " ");

        case TrackView::ColumnTitle:
            return QVariant(track->title());

        case TrackView::ColumnArtist:
            return QVariant(track->artist());

        case TrackView::ColumnAlbum:
            return QVariant(track->album());

        case TrackView::ColumnComment:
            return QVariant(track->comment());

        case TrackView::ColumnFileName:
            return QVariant(track->resultFileName());
        }

        return QVariant();
    }

    // ToolTip ::::::::::::::::::::::::::::::::::::::::::
    if (role == Qt::ToolTipRole)
    {
        switch (index.column())
        {
        case TrackView::ColumnFileName:
            return QVariant(track->resultFilePath());

        default:
            return QVariant();
        }
    }

    // TextAlignmen :::::::::::::::::::::::::::::::::::::
    if (role == Qt::TextAlignmentRole)
    {
        switch (index.column())
        {
        case TrackView::ColumnDuration:
            return  Qt::AlignRight + Qt::AlignVCenter;
        }

        return QVariant();

    }


    switch (role)
    {
    case RoleItemType:  return TrackItem;
    case RolePercent:   return     mCache->tracks[*track].percent;
    case RoleStatus:    return int(mCache->tracks[*track].state);
    case RoleTracknum:  return track->trackNum();
    case RoleDuration:  return track->duration();
    case RoleTitle:     return track->title();
    case RoleArtist:    return track->tag(TagId::AlbumArtist);
    case RoleAlbum:     return track->album();
    case RoleComment:   return track->album();
    case RoleFileName:  return track->resultFileName();
    default:            return QVariant();
    }

    return QVariant();
}


/************************************************

 ************************************************/
QVariant TrackViewModel::diskData(const Disk *disk, const QModelIndex &index, int role) const
{
    // Display & Edit :::::::::::::::::::::::::::::::::::
    if (role == Qt::DisplayRole || role == Qt::EditRole )
    {
        if (!disk->count())
            return QVariant();

        QSet<QString> values;

        switch (index.column())
        {
        case TrackView::ColumnTitle:
            for (int i=0; i<disk->count(); ++i)
                values << disk->track(i)->title();
            break;

        case TrackView::ColumnArtist:
            for (int i=0; i<disk->count(); ++i)
                values << disk->track(i)->tag(TagId::AlbumArtist);
            break;

        case TrackView::ColumnAlbum:
            for (int i=0; i<disk->count(); ++i)
                values << disk->track(i)->album();
            break;
        }

        if (values.count() > 1)
        {
            return QVariant(tr("Multiple values"));
        }
        else if (values.count() == 1)
        {
            return QVariant(*(values.begin()));
        }

        return QVariant();
    }


    switch (role)
    {
    case RoleItemType:      return DiskItem;
    case RoleTagSetTitle:   return disk->tagSetTitle();
    case RoleAudioFileName: return disk->audioFileName();
    case RoleHasWarnings:   return !disk->warnings().isEmpty();
    case RoleCanConvert:    return disk->canConvert();
    case RoleIsDownloads:   return mCache->downloadedDisks.contains(index.row());
    case RoleCoverFile:     return disk->coverImageFile();
    case RoleCoverImg:      return disk->coverImagePreview();
    case RoleCueFilePath:   return disk->cueFile();
    case RoleAudioFilePath: return disk->audioFileName();
    case RoleDiskWarnings:  return disk->warnings();
    case RoleDiskErrors:
                        {
                            QString s;
                            if (!disk->canConvert(&s))
                                return QVariant(tr("The conversion is not possible.\n%1").arg(s));
                            else
                                return QVariant();
                        }
    default:                return QVariant();
    }

    return QVariant();
}


/************************************************
 *
 ************************************************/
QString TrackViewModel::trackDurationToString(uint milliseconds) const
{
    if (milliseconds > 0)
    {
        uint l = milliseconds / 1000;
        uint h = l / 3600;
        uint m = (l % 3600) / 60;
        uint s = l % 60;


        if (h > 0)
            return tr("%1:%2:%3", "Track length, string like '01:02:56'").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
        else
            return tr("%1:%2", "Track length, string like '02:56'").arg(m).arg(s, 2, 10, QChar('0'));
    }
    else
    {
        return "??";
    }
}


/************************************************

 ************************************************/
int TrackViewModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return TrackView::ColumnCount;
}


/************************************************

 ************************************************/
int TrackViewModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return project->count();

    IndexData data(parent);
    if (data.isTrack())
        return 0;


    Disk *disk = data.disk();
    if(disk)
        return disk->count();

    return 0;
}


/************************************************

 ************************************************/
Qt::ItemFlags TrackViewModel::flags(const QModelIndex &index) const
{
    if (! index.isValid())
        return Qt::ItemIsEnabled;

    Qt::ItemFlags res = Qt::ItemIsEnabled | Qt::ItemIsSelectable;

    if (IndexData(index).isTrack())
    {
        switch (index.column())
        {
        case TrackView::ColumnTitle:
        case TrackView::ColumnArtist:
        case TrackView::ColumnAlbum:
        case TrackView::ColumnComment:
            res = res | Qt::ItemIsEditable;
            break;
        }

    }

    return res;
}


/************************************************

 ************************************************/
Disk *TrackViewModel::diskByIndex(const QModelIndex &index)
{
    return IndexData(index).disk();
}


/************************************************

 ************************************************/
Track *TrackViewModel::trackByIndex(const QModelIndex &index)
{
    return IndexData(index).track();
}


/************************************************
 *
 ************************************************/
void TrackViewModel::downloadStarted(const Disk &disk)
{
    mCache->downloadedDisks << index(disk).row();
    diskDataChanged(&disk);
}


/************************************************
 *
 ************************************************/
void TrackViewModel::downloadFinished(const Disk &disk)
{
    mCache->downloadedDisks.remove(index(disk).row());
    diskDataChanged(&disk);
}


/************************************************
 *
 ************************************************/
void TrackViewModel::trackProgressChanged(const Track &track, TrackState state, Percent percent)
{
    CacheTrackData &cache = mCache->tracks[track];
    cache.state   = state;
    cache.percent = percent;

    QModelIndex idx = index(track, TrackView::ColumnPercent);
    emit dataChanged(idx, idx);
}


/************************************************

 ************************************************/
void TrackViewModel::diskDataChanged(const Disk *disk)
{
    QModelIndex index1 = index(*disk, 0);
    QModelIndex index2 = index(*disk, TrackView::ColumnCount);
    emit dataChanged(index1, index2);
}


/************************************************

 ************************************************/
void TrackViewModel::invalidateCache(const Disk *disk)
{
    mCache->downloadedDisks.remove(index(*disk).row());

    for (int i=0; i<disk->count(); ++i)
        mCache->tracks.remove(*(disk->track(i)));
}
