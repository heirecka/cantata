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

#include "application.h"
#ifdef ENABLE_KDE_SUPPORT
#include <KDE/KUniqueApplication>
#include <KDE/KAboutData>
#include <KDE/KCmdLineArgs>
#else
#include <QTranslator>
#include <QTextCodec>
#include <QLibraryInfo>
#include <QDir>
#include <QFile>
#include <QSettings>
#endif
#include "support/utils.h"
#include "config.h"
#include "settings.h"
#include "initialsettingswizard.h"
#include "mainwindow.h"
#include "mpd-interface/song.h"
#include "support/thread.h"
#include "db/librarydb.h"

// To enable debug...
#include "mpd-interface/mpdconnection.h"
#include "mpd-interface/mpdparseutils.h"
#include "covers.h"
#include "context/wikipediaengine.h"
#include "context/lastfmengine.h"
#include "context/metaengine.h"
#include "dynamic/dynamic.h"
#ifdef ENABLE_DEVICES_SUPPORT
#include "models/devicesmodel.h"
#endif
#include "streams/streamfetcher.h"
#include "http/httpserver.h"
#include "widgets/songdialog.h"
#include "network/networkaccessmanager.h"
#include "context/ultimatelyricsprovider.h"
#ifdef ENABLE_EXTERNAL_TAGS
#include "tags/taghelperiface.h"
#endif
#include "context/contextwidget.h"
#include "scrobbling/scrobbler.h"
#ifndef ENABLE_KDE_SUPPORT
#include "gui/mediakeys.h"
#endif

#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>
#include <QDateTime>
static QMutex msgMutex;
static bool firstMsg=true;
#if QT_VERSION < 0x050000
static void cantataQtMsgHandler(QtMsgType, const char *msg)
#else
static void cantataQtMsgHandler(QtMsgType, const QMessageLogContext &, const QString &msg)
#endif
{
    QMutexLocker locker(&msgMutex);
    QFile f(Utils::cacheDir(QString(), true)+"cantata.log");
    if (f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text)) {
        QTextStream stream(&f);
        if (firstMsg) {
            stream << "------------START------------" << endl;
            firstMsg=false;
        }
        stream << QDateTime::currentDateTime().toString(Qt::ISODate).replace("T", " ") << " - " << msg << endl;
    }
}

#ifndef ENABLE_KDE_SUPPORT
// Taken from Clementine!
//
// We convert from .po files to .qm files, which loses context information.
// This translator tries loading strings with an empty context if it can't
// find any others.

class PoTranslator : public QTranslator
{
public:
    #if QT_VERSION < 0x050000
    QString translate(const char *context, const char *sourceText, const char *disambiguation = 0) const
    {
        QString ret = QTranslator::translate(context, sourceText, disambiguation);
        return !ret.isEmpty() ? ret : QTranslator::translate(NULL, sourceText, disambiguation);
    }
    #else
    QString translate(const char *context, const char *sourceText, const char *disambiguation = 0, int n=-1) const
    {
        QString ret = QTranslator::translate(context, sourceText, disambiguation, n);
        return !ret.isEmpty() ? ret : QTranslator::translate(NULL, sourceText, disambiguation, n);
    }
    #endif
};

static void loadTranslation(const QString &prefix, const QString &path, const QString &overrideLanguage = QString())
{
    #if QT_VERSION < 0x040700
    // QTranslator::load will try to open and read "cantata" if it exists,
    // without checking if it's a file first.
    // This was fixed in Qt 4.7
    QFileInfo maybeCantataDirectory(path + "/cantata");
    if (maybeCantataDirectory.exists() && !maybeCantataDirectory.isFile()) {
        return;
    }
    #endif

    QString language = overrideLanguage.isEmpty() ? QLocale::system().name() : overrideLanguage;
    QTranslator *t = new PoTranslator;
    if (t->load(prefix+"_"+language, path)) {
        QCoreApplication::installTranslator(t);
    } else {
        delete t;
    }

    #if QT_VERSION < 0x050000
    QTextCodec::setCodecForTr(QTextCodec::codecForLocale());
    #endif
}
#endif

static void removeOldFiles(const QString &d, const QStringList &types)
{
    if (!d.isEmpty()) {
        QDir dir(d);
        if (dir.exists()) {
            QFileInfoList files=dir.entryInfoList(types, QDir::Files|QDir::NoDotAndDotDot);
            foreach (const QFileInfo &file, files) {
                QFile::remove(file.absoluteFilePath());
            }
            QString dirName=dir.dirName();
            if (!dirName.isEmpty()) {
                dir.cdUp();
                dir.rmdir(dirName);
            }
        }
    }
}

static void removeOldFiles()
{
    // Remove Cantata 1.x XML cache files
    removeOldFiles(Utils::cacheDir("library"), QStringList() << "*.xml" << "*.xml.gz");
    removeOldFiles(Utils::cacheDir("jamendo"), QStringList() << "*.xml.gz");
    removeOldFiles(Utils::cacheDir("magnatune"), QStringList() << "*.xml.gz");
}

enum Debug {
    Dbg_Mpd               = 0x00000001,
    Dbg_MpdParse          = 0x00000002,
    Dbg_Covers            = 0x00000004,
    Dbg_Context_Wikipedia = 0x00000008,
    Dbg_Context_LastFm    = 0x00000010,
    Dbg_Context_Meta      = 0x00000020,
    Dbg_Context_Widget    = 0x00000040,
//    Dbg_Context_Backdrop  = 0x00000080,
    Dbg_Dynamic           = 0x00000100,
    Dbg_StreamFetching    = 0x00000200,
    Dbg_HttpServer        = 0x00000400,
    Dbg_SongDialogs       = 0x00000800,
    Dbg_NetworkAccess     = 0x00001000,
    Dbg_Context_Lyrics    = 0x00002000,
    Dbg_Threads           = 0x00004000,
    Dbg_Tags              = 0x00008000,
    Dbg_Scrobbling        = 0x00010000,
    Dbg_Devices           = 0x00020000,
    Dbg_Sql               = 0x00040000,
    DBG_Other             = 0x00080000,

    // NOTE: MUST UPDATE Dbg_All IF ADD NEW ITEMS!!!
    Dbg_All               = 0x000FFFFF
};

static void installDebugMessageHandler()
{
    QString debug=qgetenv("CANTATA_DEBUG");
    if (!debug.isEmpty()) {
        int dbg=0;
        if (debug.contains(QLatin1String("0x"))) {
            dbg=debug.startsWith(QLatin1Char('-')) ? (debug.mid(1).toInt(0, 16)*-1) : debug.toInt(0, 16);
        } else {
            dbg=debug.toInt();
        }
        bool logToFile=dbg>0;
        if (dbg<0) {
            dbg*=-1;
        }
        if (dbg&Dbg_Mpd) {
            MPDConnection::enableDebug();
        }
        if (dbg&Dbg_MpdParse) {
            MPDParseUtils::enableDebug();
        }
        if (dbg&Dbg_Covers) {
            Covers::enableDebug();
        }
        if (dbg&Dbg_Context_Wikipedia) {
            WikipediaEngine::enableDebug();
        }
        if (dbg&Dbg_Context_LastFm) {
            LastFmEngine::enableDebug();
        }
        if (dbg&Dbg_Context_Meta) {
            MetaEngine::enableDebug();
        }
        if (dbg&Dbg_Context_Widget) {
            ContextWidget::enableDebug();
        }
        if (dbg&Dbg_Dynamic) {
            Dynamic::enableDebug();
        }
        if (dbg&Dbg_StreamFetching) {
            StreamFetcher::enableDebug();
        }
        if (dbg&Dbg_HttpServer) {
            HttpServer::enableDebug();
        }
        if (dbg&Dbg_SongDialogs) {
            SongDialog::enableDebug();
        }
        if (dbg&Dbg_NetworkAccess) {
            NetworkAccessManager::enableDebug();
        }
        if (dbg&Dbg_Context_Lyrics) {
            UltimateLyricsProvider::enableDebug();
        }
        if (dbg&Dbg_Threads) {
            ThreadCleaner::enableDebug();
        }
        #ifdef ENABLE_EXTERNAL_TAGS
        if (dbg&Dbg_Tags) {
            TagHelperIface::enableDebug();
        }
        #endif
        if (dbg&Dbg_Scrobbling) {
            Scrobbler::enableDebug();
        }
        #ifdef ENABLE_DEVICES_SUPPORT
        if (dbg&Dbg_Devices) {
            DevicesModel::enableDebug();
        }
        #endif
        if (dbg&Dbg_Sql) {
            LibraryDb::enableDebug();
        }
        #ifndef ENABLE_KDE_SUPPORT
        if (dbg&DBG_Other) {
            MediaKeys::enableDebug();
        }
        #endif
        if (dbg&Dbg_All && logToFile) {
            #if QT_VERSION < 0x050000
            qInstallMsgHandler(cantataQtMsgHandler);
            #else
            qInstallMessageHandler(cantataQtMsgHandler);
            #endif
        }
    }
}

int main(int argc, char *argv[])
{
    QThread::currentThread()->setObjectName("GUI");
    #ifdef ENABLE_KDE_SUPPORT
    KAboutData aboutData(PACKAGE_NAME, 0,
                         ki18n("Cantata"), PACKAGE_VERSION_STRING,
                         ki18n("A KDE client for MPD"),
                         KAboutData::License_GPL_V3,
                         ki18n("&copy; 2011-2015 Craig Drummond"),
                         KLocalizedString(),
                         "https://github.com/CDrummond/cantata", "craig.p.drummond@gmail.com");

    aboutData.addAuthor(ki18n("Craig Drummond"), ki18n("Maintainer"), "craig.p.drummond@gmail.com");
    aboutData.addAuthor(ki18n("Piotr Wicijowski"), ki18n("UI Improvements"), "piotr.wicijowski@gmail.com");
    aboutData.addAuthor(ki18n("Sander Knopper"), ki18n("QtMPC author"), QByteArray(), "http://lowblog.nl");
    aboutData.addAuthor(ki18n("Roeland Douma"), ki18n("QtMPC author"), QByteArray(), "http://lowblog.nl");
    aboutData.addAuthor(ki18n("Daniel Selinger"), ki18n("QtMPC author"), QByteArray(), "http://lowblog.nl");
    aboutData.addAuthor(ki18n("Armin Walland"), ki18n("QtMPC author"), QByteArray(), "http://lowblog.nl");
    aboutData.addCredit(ki18n("FanArt.tv"), ki18n("Context view backdrops (please consider uploading your own music fan-art to fanart.tv)"), QByteArray(), "www.fanart.tv");
    aboutData.addCredit(ki18n("Wikipedia"), ki18n("Context view metadata"), QByteArray(), "www.wikipedia.org");
    aboutData.addCredit(ki18n("Last.fm"), ki18n("Context view metadata"), QByteArray(), "www.last.fm");
    KCmdLineArgs::init(argc, argv, &aboutData);
    #ifdef TAGLIB_FOUND
    KCmdLineOptions options;
    options.add("+[URL]", ki18n("URL to open"));
    KCmdLineArgs::addCmdLineOptions(options);
    KUniqueApplication::addCmdLineOptions();
    #endif
    if (!KUniqueApplication::start()) {
        exit(0);
    }

    Application app;
    
    removeOldFiles();
    installDebugMessageHandler();

    #else // ENABLE_KDE_SUPPORT
    QCoreApplication::setApplicationName(PACKAGE_NAME);
    QCoreApplication::setOrganizationName(ORGANIZATION_NAME);

    Application app(argc, argv);
    if (!app.start()) {
        return 0;
    }

    // Set the permissions on the config file on Unix - it can contain passwords
    // for internet services so it's important that other users can't read it.
    // On Windows these are stored in the registry instead.
    #ifdef Q_OS_UNIX
    QSettings s;

    // Create the file if it doesn't exist already
    if (!QFile::exists(s.fileName())) {
        QFile file(s.fileName());
        file.open(QIODevice::WriteOnly);
    }

    // Set -rw-------
    QFile::setPermissions(s.fileName(), QFile::ReadOwner | QFile::WriteOwner);
    #endif

    removeOldFiles();
    installDebugMessageHandler();

    // Translations
    QString lang=Settings::self()->lang();
    #if defined Q_OS_WIN || defined Q_OS_MAC
    loadTranslation("qt", CANTATA_SYS_TRANS_DIR, lang);
    #else
    loadTranslation("qt", QLibraryInfo::location(QLibraryInfo::TranslationsPath), lang);
    #endif
    loadTranslation("cantata", CANTATA_SYS_TRANS_DIR, lang);

    Application::initObjects();

    if (Settings::self()->firstRun()) {
        InitialSettingsWizard wz;
        if (QDialog::Rejected==wz.exec()) {
            return 0;
        }
    }
    MainWindow mw;
    #if defined Q_OS_WIN || defined Q_OS_MAC
    app.setActivationWindow(&mw);
    #endif // !defined Q_OS_MAC
    app.loadFiles();
    if (!Settings::self()->startHidden()) {
        mw.show();
    }
    #endif // ENABLE_KDE_SUPPORT

    return app.exec();
}
