/*
 * Cantata
 *
 * Copyright (c) 2011-2015 Craig Drummond <craig.p.drummond@gmail.com>
 *
 * ----
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "folderpage.h"
#include "mpd-interface/mpdconnection.h"
#include "settings.h"
#include "support/localize.h"
#include "support/messagebox.h"
#include "support/action.h"
#include "support/utils.h"
#include "models/mpdlibrarymodel.h"
#include "widgets/menubutton.h"
#include "stdactions.h"
#include "customactions.h"
#include <QDesktopServices>
#include <QUrl>

FolderPage::FolderPage(QWidget *p)
    : SinglePageWidget(p)
    , model(this)
{
    browseAction = new Action(Icon("system-file-manager"), i18n("Open In File Manager"), this);
    connect(view, SIGNAL(itemsSelected(bool)), this, SLOT(controlActions()));
    connect(view, SIGNAL(doubleClicked(const QModelIndex &)), this, SLOT(itemDoubleClicked(const QModelIndex &)));
    connect(browseAction, SIGNAL(triggered()), this, SLOT(openFileManager()));
    connect(MPDConnection::self(), SIGNAL(updatingFileList()), view, SLOT(updating()));
    connect(MPDConnection::self(), SIGNAL(updatedFileList()), view, SLOT(updated()));
    connect(MPDConnection::self(), SIGNAL(updatingDatabase()), view, SLOT(updating()));
    connect(MPDConnection::self(), SIGNAL(updatedDatabase()), view, SLOT(updated()));
    Configuration config(metaObject()->className());
    view->setMode(ItemView::Mode_DetailedTree);
    view->load(config);
    MenuButton *menu=new MenuButton(this);
    menu->addActions(createViewActions(QList<ItemView::Mode>() << ItemView::Mode_BasicTree << ItemView::Mode_SimpleTree
                                                               << ItemView::Mode_DetailedTree << ItemView::Mode_List));
    init(ReplacePlayQueue|AppendToPlayQueue, QList<QWidget *>() << menu);

    view->addAction(StdActions::self()->addToStoredPlaylistAction);
    view->addAction(CustomActions::self());
    #ifdef TAGLIB_FOUND
    #ifdef ENABLE_DEVICES_SUPPORT
    view->addAction(StdActions::self()->copyToDeviceAction);
    #endif
    view->addAction(StdActions::self()->organiseFilesAction);
    view->addAction(StdActions::self()->editTagsAction);
    #ifdef ENABLE_REPLAYGAIN_SUPPORT
    view->addAction(StdActions::self()->replaygainAction);
    #endif // TAGLIB_FOUND
    #endif
    view->addAction(browseAction);
    #ifdef ENABLE_DEVICES_SUPPORT
    view->addSeparator();
    view->addAction(StdActions::self()->deleteSongsAction);
    #endif
    view->setModel(&model);
    connect(view, SIGNAL(updateToPlayQueue(QModelIndex,bool)), this, SLOT(updateToPlayQueue(QModelIndex,bool)));
}

FolderPage::~FolderPage()
{
    Configuration config(metaObject()->className());
    view->save(config);
}

void FolderPage::showEvent(QShowEvent *e)
{
    view->focusView();
    SinglePageWidget::showEvent(e);
    model.load();
}

void FolderPage::controlActions()
{
    QModelIndexList selected=view->selectedIndexes(false); // Dont need sorted selection here...
    bool enable=selected.count()>0;
    bool trackSelected=false;
    bool folderSelected=false;

    foreach (const QModelIndex &idx, selected) {
        if (static_cast<BrowseModel::Item *>(idx.internalPointer())->isFolder()) {
            folderSelected=true;
        } else {
            trackSelected=true;
        }
        if (folderSelected && trackSelected) {
            enable=false;
            break;
        }
    }

    StdActions::self()->enableAddToPlayQueue(enable);
    StdActions::self()->addToStoredPlaylistAction->setEnabled(enable);
    #ifdef TAGLIB_FOUND
    StdActions::self()->organiseFilesAction->setEnabled(enable && trackSelected && MPDConnection::self()->getDetails().dirReadable);
    StdActions::self()->editTagsAction->setEnabled(StdActions::self()->organiseFilesAction->isEnabled());
    #ifdef ENABLE_REPLAYGAIN_SUPPORT
    StdActions::self()->replaygainAction->setEnabled(StdActions::self()->organiseFilesAction->isEnabled());
    #endif
    #ifdef ENABLE_DEVICES_SUPPORT
    StdActions::self()->deleteSongsAction->setEnabled(StdActions::self()->organiseFilesAction->isEnabled());
    StdActions::self()->copyToDeviceAction->setEnabled(StdActions::self()->organiseFilesAction->isEnabled());
    #endif
    #endif // TAGLIB_FOUND

    browseAction->setEnabled(enable && 1==selected.count() && folderSelected);
}

void FolderPage::itemDoubleClicked(const QModelIndex &)
{
    const QModelIndexList selected = view->selectedIndexes(false); // Dont need sorted selection here...
    if (1!=selected.size()) {
        return; //doubleclick should only have one selected item
    }

    if (!static_cast<BrowseModel::Item *>(selected.at(0).internalPointer())->isFolder()) {
        addSelectionToPlaylist();
    }
}

void FolderPage::openFileManager()
{
    const QModelIndexList selected = view->selectedIndexes(false); // Dont need sorted selection here...
    if (1!=selected.size()) {
        return;
    }

    BrowseModel::Item *item = static_cast<BrowseModel::Item *>(selected.at(0).internalPointer());
    if (item->isFolder()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(MPDConnection::self()->getDetails().dir+static_cast<BrowseModel::FolderItem *>(item)->getPath()));
    }
}

void FolderPage::updateToPlayQueue(const QModelIndex &idx, bool replace)
{
    BrowseModel::Item *item = static_cast<BrowseModel::Item *>(idx.internalPointer());
    if (item->isFolder()) {
        emit add(QStringList() << MPDConnection::constDirPrefix+static_cast<BrowseModel::FolderItem *>(item)->getPath(),
                 replace ? MPDConnection::ReplaceAndplay : MPDConnection::Append, 0);
    }
}

QList<Song> FolderPage::selectedSongs(bool allowPlaylists) const
{
    return model.songs(view->selectedIndexes(), allowPlaylists);
}

QStringList FolderPage::selectedFiles(bool allowPlaylists) const
{
    QList<Song> songs=selectedSongs(allowPlaylists);
    QStringList files;
    foreach (const Song &s, songs) {
        files.append(s.file);
    }
    return files;
}

void FolderPage::addSelectionToPlaylist(const QString &name, int action, quint8 priorty)
{
    QModelIndexList selected=view->selectedIndexes();
    QStringList dirs;
    QStringList files;

    foreach (const QModelIndex &idx, selected) {
        if (static_cast<BrowseModel::Item *>(idx.internalPointer())->isFolder()) {
            files+=static_cast<BrowseModel::FolderItem *>(idx.internalPointer())->allEntries();
        } else {
            files.append(static_cast<BrowseModel::TrackItem *>(idx.internalPointer())->getSong().file);
        }
    }

    if (!files.isEmpty()) {
        if (name.isEmpty()) {
            emit add(files, action, priorty);
        } else {
            emit addSongsToPlaylist(name, files);
        }
        view->clearSelection();
    }
}

#ifdef ENABLE_DEVICES_SUPPORT
void FolderPage::addSelectionToDevice(const QString &udi)
{
    QList<Song> songs=selectedSongs();

    if (!songs.isEmpty()) {
        emit addToDevice(QString(), udi, songs);
        view->clearSelection();
    }
}

void FolderPage::deleteSongs()
{
    QList<Song> songs=selectedSongs();

    if (!songs.isEmpty()) {
        if (MessageBox::Yes==MessageBox::warningYesNo(this, i18n("Are you sure you wish to delete the selected songs?\n\nThis cannot be undone."),
                                                      i18n("Delete Songs"), StdGuiItem::del(), StdGuiItem::cancel())) {
            emit deleteSongs(QString(), songs);
        }
        view->clearSelection();
    }
}
#endif
