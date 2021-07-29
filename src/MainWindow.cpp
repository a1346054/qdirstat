/*
 *   File name: MainWindow.cpp
 *   Summary:	QDirStat main window
 *   License:	GPL V2 - See file LICENSE for details.
 *
 *   Author:	Stefan Hundhammer <Stefan.Hundhammer@gmx.de>
 */


#include <QApplication>
#include <QCloseEvent>
#include <QMessageBox>
#include <QMenu>
#include <QFileDialog>
#include <QSignalMapper>
#include <QClipboard>

#include "MainWindow.h"
#include "ActionManager.h"
#include "BusyPopup.h"
#include "CleanupCollection.h"
#include "CleanupConfigPage.h"
#include "ConfigDialog.h"
#include "DataColumns.h"
#include "DebugHelpers.h"
#include "DirTree.h"
#include "DirTreeCache.h"
#include "DirTreeModel.h"
#include "DirTreePatternFilter.h"
#include "DirTreePkgFilter.h"
#include "Exception.h"
#include "ExcludeRules.h"
#include "FileDetailsView.h"
#include "FileInfo.h"
#include "FileSizeStatsWindow.h"
#include "HeaderTweaker.h"
#include "Logger.h"
#include "MimeCategorizer.h"
#include "MimeCategoryConfigPage.h"
#include "OpenDirDialog.h"
#include "OpenPkgDialog.h"
#include "OutputWindow.h"
#include "PanelMessage.h"
#include "PkgManager.h"
#include "PkgQuery.h"
#include "Refresher.h"
#include "SelectionModel.h"
#include "Settings.h"
#include "SettingsHelpers.h"
#include "ShowUnpkgFilesDialog.h"
#include "SysUtil.h"
#include "Trash.h"
#include "Version.h"

#define LONG_MESSAGE		25*1000
#define UPDATE_MILLISEC		200

#if (QT_VERSION < QT_VERSION_CHECK( 5, 13, 0 ))
#  define HAVE_SIGNAL_MAPPER	  1
#else
// QSignalMapper is deprecated from Qt 5.13 on
#  define HAVE_SIGNAL_MAPPER	  0
#endif

#define USE_CUSTOM_OPEN_DIR_DIALOG 1

using namespace QDirStat;

using QDirStat::DataColumns;
using QDirStat::DirTreeModel;
using QDirStat::SelectionModel;
using QDirStat::CleanupCollection;
using QDirStat::PkgFilter;


MainWindow::MainWindow():
    QMainWindow(),
    _ui( new Ui::MainWindow ),
    _configDialog(0),
    _modified( false ),
    _enableDirPermissionsWarning( false ),
    _verboseSelection( false ),
    _urlInWindowTitle( false ),
    _useTreemapHover( false ),
    _statusBarTimeout( 3000 ), // millisec
    _treeLevelMapper(0),
    _currentLayout( 0 )
{
    CHECK_PTR( _ui );

    _ui->setupUi( this );
    ActionManager::instance()->addWidgetTree( this );
    initLayoutActions();
    createLayouts();
    readSettings();
    _updateTimer.setInterval( UPDATE_MILLISEC );
    _treeExpandTimer.setSingleShot( true );
    _dUrl = _ui->actionDonate->iconText();
    _futureSelection.setUseRootFallback( false );

    _dirTreeModel = new DirTreeModel( this );
    CHECK_NEW( _dirTreeModel );

    _selectionModel = new SelectionModel( _dirTreeModel, this );
    CHECK_NEW( _selectionModel );

    _ui->dirTreeView->setModel( _dirTreeModel );
    _ui->dirTreeView->setSelectionModel( _selectionModel );

    _ui->treemapView->setDirTree( _dirTreeModel->tree() );
    _ui->treemapView->setSelectionModel( _selectionModel );

    _dirTreeModel->setSelectionModel( _selectionModel );

    _cleanupCollection = new CleanupCollection( _selectionModel );
    CHECK_NEW( _cleanupCollection );

    _cleanupCollection->addToMenu   ( _ui->menuCleanup,
				      true ); // keepUpdated
    _cleanupCollection->addToToolBar( _ui->toolBar,
				      true ); // keepUpdated


    _ui->dirTreeView->setCleanupCollection( _cleanupCollection );
    _ui->treemapView->setCleanupCollection( _cleanupCollection );

    _ui->breadcrumbNavigator->clear();

    _history = new History();
    CHECK_NEW( _history );

    initHistoryButtons();

#ifdef Q_OS_MACX
    // This makes the application to look like more "native" on macOS
    setUnifiedTitleAndToolBarOnMac( true );
    _ui->toolBar->setMovable( false );
#endif

    connectSignals();
    connectActions();
    changeLayout( _layoutName );

    if ( ! PkgQuery::haveGetInstalledPkgSupport() ||
	 ! PkgQuery::haveFileListSupport()	    )
    {
	logInfo() << "No package manager support "
		  << "for getting installed packages or file lists"
		  << endl;

	_ui->actionOpenPkg->setEnabled( false );
    }

    PkgManager * pkgManager = PkgQuery::primaryPkgManager();

    if ( ! pkgManager || ! pkgManager->supportsFileListCache() )
    {
	logInfo() << "No package manager support "
		  << "for getting a file lists cache"
		  << endl;

	_ui->actionShowUnpkgFiles->setEnabled( false );
    }

    if ( ! _ui->actionShowTreemap->isChecked() )
	_ui->treemapView->disable();

    toggleVerboseSelection();
    updateActions();
}


MainWindow::~MainWindow()
{
    if ( _currentLayout )
	saveLayout( _currentLayout );

    writeSettings();
    ExcludeRules::instance()->writeSettings();
    MimeCategorizer::instance()->writeSettings();

    // Relying on the QObject hierarchy to properly clean this up resulted in a
    //	segfault; there was probably a problem in the deletion order.

    if ( _configDialog )
	delete _configDialog;

    delete _ui->dirTreeView;
    delete _ui;
    delete _cleanupCollection;
    delete _selectionModel;
    delete _dirTreeModel;
    delete _history;

    qDeleteAll( _layouts );
}


void MainWindow::connectSignals()
{
    connect( _selectionModel,		SIGNAL( currentBranchChanged( QModelIndex ) ),
	     _ui->dirTreeView,		SLOT  ( closeAllExcept	    ( QModelIndex ) ) );

    connect( _dirTreeModel->tree(),	SIGNAL( startingReading() ),
	     this,			SLOT  ( startingReading() ) );

    connect( _dirTreeModel->tree(),	SIGNAL( finished()	  ),
	     this,			SLOT  ( readingFinished() ) );

    connect( _dirTreeModel->tree(),	SIGNAL( aborted()	  ),
	     this,			SLOT  ( readingAborted()  ) );

    connect( _selectionModel,		SIGNAL( selectionChanged() ),
	     this,			SLOT  ( updateActions()	   ) );

    connect( _selectionModel,		SIGNAL( currentItemChanged( FileInfo *, FileInfo * ) ),
	     this,			SLOT  ( updateActions()				    ) );

    connect( _selectionModel,		SIGNAL( currentItemChanged( FileInfo *, FileInfo * ) ),
	     _ui->breadcrumbNavigator,	SLOT  ( setPath		  ( FileInfo *		   ) ) );

    connect( _selectionModel,		SIGNAL( currentItemChanged( FileInfo *, FileInfo * ) ),
	     this,                      SLOT  ( addToHistory      ( FileInfo *		   ) ) );

    connect( _ui->breadcrumbNavigator,	SIGNAL( pathClicked   ( QString ) ),
	     _selectionModel,		SLOT  ( setCurrentItem( QString ) ) );

    connect( _ui->treemapView,		SIGNAL( treemapChanged() ),
	     this,			SLOT  ( updateActions()	 ) );

    connect( _cleanupCollection,	SIGNAL( startingCleanup( QString ) ),
	     this,			SLOT  ( startingCleanup( QString ) ) );

    connect( _cleanupCollection,	SIGNAL( cleanupFinished( int ) ),
	     this,			SLOT  ( cleanupFinished( int ) ) );

    connect( &_updateTimer,		SIGNAL( timeout()	  ),
	     this,			SLOT  ( showElapsedTime() ) );

    connect( &_treeExpandTimer,		  SIGNAL( timeout()	  ),
             _ui->actionExpandTreeLevel1, SLOT( trigger()          ) );

    if ( _useTreemapHover )
    {
	connect( _ui->treemapView,	SIGNAL( hoverEnter ( FileInfo * ) ),
		 this,			SLOT  ( showCurrent( FileInfo * ) ) );

	connect( _ui->treemapView,	SIGNAL( hoverLeave ( FileInfo * ) ),
		 this,			SLOT  ( showSummary()		) );
    }

    // Debug connections

    connect( _ui->dirTreeView,		SIGNAL( clicked	   ( QModelIndex ) ),
	     this,			SLOT  ( itemClicked( QModelIndex ) ) );

    connect( _selectionModel,		SIGNAL( selectionChanged() ),
	     this,			SLOT  ( selectionChanged() ) );

    connect( _selectionModel,		SIGNAL( currentItemChanged( FileInfo *, FileInfo * ) ),
	     this,			SLOT  ( currentItemChanged( FileInfo *, FileInfo * ) ) );
}


#define CONNECT_ACTION(ACTION, RECEIVER, RCVR_SLOT) \
    connect( (ACTION), SIGNAL( triggered() ), (RECEIVER), SLOT( RCVR_SLOT ) )


void MainWindow::connectActions()
{
    // "File" menu

    CONNECT_ACTION( _ui->actionOpenDir,			    this, askOpenDir()	      );
    CONNECT_ACTION( _ui->actionOpenPkg,			    this, askOpenPkg()	      );
    CONNECT_ACTION( _ui->actionShowUnpkgFiles,		    this, askShowUnpkgFiles() );
    CONNECT_ACTION( _ui->actionRefreshAll,		    this, refreshAll()	      );
    CONNECT_ACTION( _ui->actionRefreshSelected,		    this, refreshSelected()   );
    CONNECT_ACTION( _ui->actionReadExcludedDirectory,	    this, refreshSelected()   );
    CONNECT_ACTION( _ui->actionContinueReadingAtMountPoint, this, refreshSelected()   );
    CONNECT_ACTION( _ui->actionStopReading,		    this, stopReading()	      );
    CONNECT_ACTION( _ui->actionAskWriteCache,		    this, askWriteCache()     );
    CONNECT_ACTION( _ui->actionAskReadCache,		    this, askReadCache()      );
    CONNECT_ACTION( _ui->actionQuit,			    qApp, quit()	      );


    // "Edit" menu

    CONNECT_ACTION( _ui->actionCopyPathToClipboard, this, copyCurrentPathToClipboard() );
    CONNECT_ACTION( _ui->actionMoveToTrash,	    this, moveToTrash()                );
    CONNECT_ACTION( _ui->actionConfigure,           this, openConfigDialog()           );


    // "View" menu

#if HAVE_SIGNAL_MAPPER

    // QSignalMapper is deprecated from Qt 5.13 on.
    // On systems with older versions, there may or may not be C++11 compiler.

    _treeLevelMapper = new QSignalMapper( this );

    connect( _treeLevelMapper, SIGNAL( mapped		( int ) ),
	     this,	       SLOT  ( expandTreeToLevel( int ) ) );

    mapTreeExpandAction( _ui->actionExpandTreeLevel0, 0 );
    mapTreeExpandAction( _ui->actionExpandTreeLevel1, 1 );
    mapTreeExpandAction( _ui->actionExpandTreeLevel2, 2 );
    mapTreeExpandAction( _ui->actionExpandTreeLevel3, 3 );
    mapTreeExpandAction( _ui->actionExpandTreeLevel4, 4 );
    mapTreeExpandAction( _ui->actionExpandTreeLevel5, 5 );
    mapTreeExpandAction( _ui->actionExpandTreeLevel6, 6 );
    mapTreeExpandAction( _ui->actionExpandTreeLevel7, 7 );
    mapTreeExpandAction( _ui->actionExpandTreeLevel8, 8 );
    mapTreeExpandAction( _ui->actionExpandTreeLevel9, 9 );

    mapTreeExpandAction( _ui->actionCloseAllTreeLevels, 0 );

#else   // QSignalMapper not available / deprecated? (Qt 5.13 or later) -> use a C++11 lambda

    connect( _ui->actionExpandTreeLevel0,   &QAction::triggered, [=]() { expandTreeToLevel( 0 ); } );
    connect( _ui->actionExpandTreeLevel1,   &QAction::triggered, [=]() { expandTreeToLevel( 1 ); } );
    connect( _ui->actionExpandTreeLevel2,   &QAction::triggered, [=]() { expandTreeToLevel( 2 ); } );
    connect( _ui->actionExpandTreeLevel3,   &QAction::triggered, [=]() { expandTreeToLevel( 3 ); } );
    connect( _ui->actionExpandTreeLevel4,   &QAction::triggered, [=]() { expandTreeToLevel( 4 ); } );
    connect( _ui->actionExpandTreeLevel5,   &QAction::triggered, [=]() { expandTreeToLevel( 5 ); } );
    connect( _ui->actionExpandTreeLevel6,   &QAction::triggered, [=]() { expandTreeToLevel( 6 ); } );
    connect( _ui->actionExpandTreeLevel7,   &QAction::triggered, [=]() { expandTreeToLevel( 7 ); } );
    connect( _ui->actionExpandTreeLevel8,   &QAction::triggered, [=]() { expandTreeToLevel( 8 ); } );
    connect( _ui->actionExpandTreeLevel9,   &QAction::triggered, [=]() { expandTreeToLevel( 9 ); } );

    connect( _ui->actionCloseAllTreeLevels, &QAction::triggered, [=]() { expandTreeToLevel( 0 ); } );

#endif

    connect( _ui->actionShowCurrentPath,  SIGNAL( toggled   ( bool ) ),
	     _ui->breadcrumbNavigator,	  SLOT	( setVisible( bool ) ) );

    connect( _ui->actionShowDetailsPanel, SIGNAL( toggled   ( bool ) ),
	     _ui->fileDetailsPanel,	  SLOT	( setVisible( bool ) ) );

    CONNECT_ACTION( _ui->actionLayout1,		   this, changeLayout() );
    CONNECT_ACTION( _ui->actionLayout2,		   this, changeLayout() );
    CONNECT_ACTION( _ui->actionLayout3,		   this, changeLayout() );

    CONNECT_ACTION( _ui->actionFileSizeStats,	   this, showFileSizeStats() );
    CONNECT_ACTION( _ui->actionFileTypeStats,	   this, showFileTypeStats() );

    _ui->actionFileTypeStats->setShortcutContext( Qt::ApplicationShortcut );

    CONNECT_ACTION( _ui->actionFileAgeStats,	   this, showFileAgeStats()  );
    CONNECT_ACTION( _ui->actionShowFilesystems,	   this, showFilesystems()   );


    // "View" -> "Treemap" submenu

    connect( _ui->actionShowTreemap, SIGNAL( toggled( bool )   ),
	     this,		     SLOT  ( showTreemapView() ) );

    connect( _ui->actionTreemapAsSidePanel, SIGNAL( toggled( bool )	 ),
	     this,			    SLOT  ( treemapAsSidePanel() ) );

    CONNECT_ACTION( _ui->actionTreemapZoomIn,	 _ui->treemapView, zoomIn()	    );
    CONNECT_ACTION( _ui->actionTreemapZoomOut,	 _ui->treemapView, zoomOut()	    );
    CONNECT_ACTION( _ui->actionResetTreemapZoom, _ui->treemapView, resetZoom()	    );
    CONNECT_ACTION( _ui->actionTreemapRebuild,	 _ui->treemapView, rebuildTreemap() );


    // "Go" menu

    CONNECT_ACTION( _ui->actionGoBack,	     this, historyGoBack() );
    CONNECT_ACTION( _ui->actionGoForward,    this, historyGoForward() );
    CONNECT_ACTION( _ui->actionGoUp,	     this, navigateUp() );
    CONNECT_ACTION( _ui->actionGoToToplevel, this, navigateToToplevel() );


    // "Discover" menu

    CONNECT_ACTION( _ui->actionDiscoverLargestFiles,    this, discoverLargestFiles()    );
    CONNECT_ACTION( _ui->actionDiscoverNewestFiles,     this, discoverNewestFiles()     );
    CONNECT_ACTION( _ui->actionDiscoverOldestFiles,     this, discoverOldestFiles()     );
    CONNECT_ACTION( _ui->actionDiscoverHardLinkedFiles, this, discoverHardLinkedFiles() );
    CONNECT_ACTION( _ui->actionDiscoverBrokenSymLinks,  this, discoverBrokenSymLinks()  );
    CONNECT_ACTION( _ui->actionDiscoverSparseFiles,     this, discoverSparseFiles()     );


    // "Help" menu

    _ui->actionWhatsNew->setStatusTip( RELEASE_URL ); // defined in Version.h

    CONNECT_ACTION( _ui->actionHelp,		this, openActionUrl()	  );
    CONNECT_ACTION( _ui->actionPkgViewHelp,	this, openActionUrl()	  );
    CONNECT_ACTION( _ui->actionUnpkgViewHelp,	this, openActionUrl()     );
    CONNECT_ACTION( _ui->actionWhatsNew,	this, openActionUrl()	  );

    CONNECT_ACTION( _ui->actionAbout,		this, showAboutDialog()	  );
    CONNECT_ACTION( _ui->actionAboutQt,		qApp, aboutQt()		  );
    CONNECT_ACTION( _ui->actionDonate,		this, showDonateDialog()  );


    // Connect all actions of submenu "Help" -> "Problems and Solutions"
    // to display the URL that they have in their statusTip property in a browser

    foreach ( QAction * action, _ui->menuProblemsAndSolutions->actions() )
    {
        QString url = action->statusTip();

        if ( url.isEmpty() )
            logWarning() << "No URL in statusTip property of action " << action->objectName() << endl;
        else
            CONNECT_ACTION( action, this, openActionUrl() );
    }


    // Invisible debug actions

    addAction( _ui->actionVerboseSelection );    // Shift-F7
    addAction( _ui->actionDumpSelection );       // F7

    connect( _ui->actionVerboseSelection, SIGNAL( toggled( bool )	   ),
	     this,			  SLOT	( toggleVerboseSelection() ) );

    CONNECT_ACTION( _ui->actionDumpSelection, _selectionModel, dumpSelectedItems() );
}


void MainWindow::mapTreeExpandAction( QAction * action, int level )
{
    if ( _treeLevelMapper )
    {
	CONNECT_ACTION( action, _treeLevelMapper, map() );
	_treeLevelMapper->setMapping( action, level );
    }
}


void MainWindow::updateActions()
{
    bool reading	     = _dirTreeModel->tree()->isBusy();
    FileInfo * currentItem   = _selectionModel->currentItem();
    FileInfo * firstToplevel = _dirTreeModel->tree()->firstToplevel();
    bool pkgView	     = firstToplevel && firstToplevel->isPkgInfo();

    _ui->actionStopReading->setEnabled( reading );
    _ui->actionRefreshAll->setEnabled	( ! reading );
    _ui->actionAskReadCache->setEnabled ( ! reading );
    _ui->actionAskWriteCache->setEnabled( ! reading );

    _ui->actionCopyPathToClipboard->setEnabled( currentItem );
    _ui->actionGoUp->setEnabled( currentItem && currentItem->treeLevel() > 1 );
    _ui->actionGoToToplevel->setEnabled( firstToplevel && ( ! currentItem || currentItem->treeLevel() > 1 ));

    FileInfoSet selectedItems = _selectionModel->selectedItems();
    FileInfo * sel	      = selectedItems.first();
    int selSize		      = selectedItems.size();

    bool oneDirSelected	   = selSize == 1 && sel && sel->isDir() && ! sel->isPkgInfo();
    bool pseudoDirSelected = selectedItems.containsPseudoDir();
    bool pkgSelected	   = selectedItems.containsPkg();

    _ui->actionMoveToTrash->setEnabled( sel && ! pseudoDirSelected && ! pkgSelected && ! reading );
    _ui->actionRefreshSelected->setEnabled( selSize == 1 && ! sel->isExcluded() && ! sel->isMountPoint() && ! pkgView );
    _ui->actionContinueReadingAtMountPoint->setEnabled( oneDirSelected && sel->isMountPoint() );
    _ui->actionReadExcludedDirectory->setEnabled      ( oneDirSelected && sel->isExcluded()   );

    bool nothingOrOneDir = selectedItems.isEmpty() || oneDirSelected;

    _ui->actionFileSizeStats->setEnabled( ! reading && nothingOrOneDir );
    _ui->actionFileTypeStats->setEnabled( ! reading && nothingOrOneDir );
    _ui->actionFileAgeStats->setEnabled ( ! reading && nothingOrOneDir );

    bool showingTreemap = _ui->treemapView->isVisible();

    _ui->actionTreemapAsSidePanel->setEnabled( showingTreemap );
    _ui->actionTreemapZoomIn->setEnabled   ( showingTreemap && _ui->treemapView->canZoomIn() );
    _ui->actionTreemapZoomOut->setEnabled  ( showingTreemap && _ui->treemapView->canZoomOut() );
    _ui->actionResetTreemapZoom->setEnabled( showingTreemap && _ui->treemapView->canZoomOut() );
    _ui->actionTreemapRebuild->setEnabled  ( showingTreemap );

    updateHistoryActions();
}


void MainWindow::readSettings()
{
    QDirStat::Settings settings;
    settings.beginGroup( "MainWindow" );

    _statusBarTimeout	  = settings.value( "StatusBarTimeoutMillisec", 3000  ).toInt();
    bool showTreemap	  = settings.value( "ShowTreemap"	      , true  ).toBool();
    bool treemapOnSide	  = settings.value( "TreemapOnSide"	      , false ).toBool();

    _verboseSelection	  = settings.value( "VerboseSelection"	      , false ).toBool();
    _urlInWindowTitle	  = settings.value( "UrlInWindowTitle"	      , false ).toBool();
    _useTreemapHover	  = settings.value( "UseTreemapHover"	      , false ).toBool();
    _layoutName		  = settings.value( "Layout"		      , "L2"  ).toString();

    settings.endGroup();

    settings.beginGroup( "MainWindow-Subwindows" );
    QByteArray mainSplitterState = settings.value( "MainSplitter" , QByteArray() ).toByteArray();
    QByteArray topSplitterState	 = settings.value( "TopSplitter"  , QByteArray() ).toByteArray();
    settings.endGroup();

    _ui->actionShowTreemap->setChecked( showTreemap );
    _ui->actionTreemapAsSidePanel->setChecked( treemapOnSide );
    treemapAsSidePanel();

    _ui->actionVerboseSelection->setChecked( _verboseSelection );

    foreach ( QAction * action, _layoutActionGroup->actions() )
    {
	if ( action->data().toString() == _layoutName )
	    action->setChecked( true );
    }

    readWindowSettings( this, "MainWindow" );

    if ( ! mainSplitterState.isNull() )
	_ui->mainWinSplitter->restoreState( mainSplitterState );

    if ( ! topSplitterState.isNull() )
	_ui->topViewsSplitter->restoreState( topSplitterState );
    else
    {
	// The Qt designer refuses to let me set a reasonable size for that
	// widget, so let's set one here. Yes, that's not really how this is
	// supposed to be, but I am fed up with that stuff.

	_ui->fileDetailsPanel->resize( QSize( 300, 300 ) );
    }

    foreach ( TreeLayout * layout, _layouts )
	readLayoutSettings( layout );

    ExcludeRules::instance()->readSettings();
    Debug::dumpExcludeRules();
}


void MainWindow::readLayoutSettings( TreeLayout * layout )
{
    CHECK_PTR( layout );

    Settings settings;
    settings.beginGroup( QString( "TreeViewLayout_%1" ).arg( layout->name ) );

    layout->showCurrentPath  = settings.value( "ShowCurrentPath" , layout->showCurrentPath  ).toBool();
    layout->showDetailsPanel = settings.value( "ShowDetailsPanel", layout->showDetailsPanel ).toBool();

    settings.endGroup();
}


void MainWindow::writeSettings()
{
    QDirStat::Settings settings;
    settings.beginGroup( "MainWindow" );

    settings.setValue( "ShowTreemap"	 , _ui->actionShowTreemap->isChecked() );
    settings.setValue( "TreemapOnSide"	 , _ui->actionTreemapAsSidePanel->isChecked() );
    settings.setValue( "VerboseSelection", _verboseSelection );
    settings.setValue( "Layout"		 , _layoutName );

    // Those are only set if not already in the settings (they might have been
    // set from a config dialog).
    settings.setDefaultValue( "StatusBarTimeoutMillisec", _statusBarTimeout );
    settings.setDefaultValue( "UrlInWindowTitle"	, _urlInWindowTitle );
    settings.setDefaultValue( "UseTreemapHover"		, _useTreemapHover );

    settings.endGroup();

    writeWindowSettings( this, "MainWindow" );

    settings.beginGroup( "MainWindow-Subwindows" );
    settings.setValue( "MainSplitter"		 , _ui->mainWinSplitter->saveState()  );
    settings.setValue( "TopSplitter"		 , _ui->topViewsSplitter->saveState() );
    settings.endGroup();

    foreach ( TreeLayout * layout, _layouts )
	writeLayoutSettings( layout );
}


void MainWindow::writeLayoutSettings( TreeLayout * layout )
{
    CHECK_PTR( layout );

    Settings settings;
    settings.beginGroup( QString( "TreeViewLayout_%1" ).arg( layout->name ) );

    settings.setValue( "ShowCurrentPath" , layout->showCurrentPath  );
    settings.setValue( "ShowDetailsPanel", layout->showDetailsPanel );

    settings.endGroup();
}


void MainWindow::showTreemapView()
{
    if ( _ui->actionShowTreemap->isChecked() )
	_ui->treemapView->enable();
    else
	_ui->treemapView->disable();
}


void MainWindow::treemapAsSidePanel()
{
    if ( _ui->actionTreemapAsSidePanel->isChecked() )
	_ui->mainWinSplitter->setOrientation(Qt::Horizontal);
    else
	_ui->mainWinSplitter->setOrientation(Qt::Vertical);
}


void MainWindow::closeEvent( QCloseEvent *event )
{
    if ( _modified )
    {
	int button = QMessageBox::question( this, tr( "Unsaved changes" ),
					    tr( "Save changes?" ),
					    QMessageBox::Save |
					    QMessageBox::Discard |
					    QMessageBox::Cancel );

	if ( button == QMessageBox::Cancel )
	{
	    event->ignore();
	    return;
	}

	if ( button == QMessageBox::Save )
	{
	    // saveFile();
	}

	event->accept();
    }
    else
    {
	event->accept();
    }
}


void MainWindow::busyDisplay()
{
    _ui->treemapView->disable();
    updateActions();

    if ( _unreadableDirsWindow )
    {
	// Close the window that lists unreadable directories: With the next
	// directory read, things might have changed; the user may have fixed
	// permissions or ownership of those directories.
	//
	// Closing this window also deletes it (because it has the
	// DeleteOnClose flag set). The QPointer we use will take care of
	// resetting itself to 0 when the underlying QObject is deleted.

	_unreadableDirsWindow->close();
    }

    _updateTimer.start();

    // It would be nice to sort by read jobs during reading, but this confuses
    // the hell out of the Qt side of the data model; so let's sort by name
    // instead.

    int sortCol = QDirStat::DataColumns::toViewCol( QDirStat::NameCol );
    _ui->dirTreeView->sortByColumn( sortCol, Qt::AscendingOrder );

    if ( ! PkgFilter::isPkgUrl( _dirTreeModel->tree()->url() ) &&
	 ! _selectionModel->currentBranch() )
    {
        _treeExpandTimer.start( 200 );
        // This will trigger actionExpandTreeLevel1. Hopefully after those 200
        // millisec there will be some items in the tree to expand.
    }
}


void MainWindow::idleDisplay()
{
    logInfo() << endl;

    updateActions();
    _updateTimer.stop();
    int sortCol = QDirStat::DataColumns::toViewCol( QDirStat::PercentNumCol );
    _ui->dirTreeView->sortByColumn( sortCol, Qt::DescendingOrder );

    if ( ! _futureSelection.isEmpty() )
    {
        _treeExpandTimer.stop();
        applyFutureSelection();
    }
    else if ( ! _selectionModel->currentBranch() )
    {
	logDebug() << "No current branch - expanding tree to level 1" << endl;
	expandTreeToLevel( 1 );
    }

    updateFileDetailsView();
    showTreemapView();
}


void MainWindow::startingReading()
{
    _stopWatch.start();
    busyDisplay();
}


void MainWindow::readingFinished()
{
    logInfo() << endl;

    idleDisplay();

    QString elapsedTime = formatMillisecTime( _stopWatch.elapsed() );
    _ui->statusBar->showMessage( tr( "Finished. Elapsed time: %1").arg( elapsedTime ), LONG_MESSAGE );
    logInfo() << "Reading finished after " << elapsedTime << endl;

    if ( _dirTreeModel->tree()->firstToplevel() &&
	 _dirTreeModel->tree()->firstToplevel()->errSubDirCount() > 0 )
    {
	showDirPermissionsWarning();
    }

    // Debug::dumpModelTree( _dirTreeModel, QModelIndex(), "" );
}


void MainWindow::readingAborted()
{
    logInfo() << endl;

    idleDisplay();
    QString elapsedTime = formatMillisecTime( _stopWatch.elapsed() );
    _ui->statusBar->showMessage( tr( "Aborted. Elapsed time: %1").arg( elapsedTime ), LONG_MESSAGE );
    logInfo() << "Reading aborted after " << elapsedTime << endl;
}


void MainWindow::openUrl( const QString & url )
{
    _enableDirPermissionsWarning = true;
    _history->clear();

    if ( PkgFilter::isPkgUrl( url ) )
	readPkg( url );
    else if ( isUnpkgUrl( url ) )
	showUnpkgFiles( url );
    else
	openDir( url );
}


void MainWindow::openDir( const QString & url )
{
    try
    {
	_dirTreeModel->openUrl( url );
	updateWindowTitle( _dirTreeModel->tree()->url() );
    }
    catch ( const SysCallFailedException & ex )
    {
	CAUGHT( ex );
	updateWindowTitle( "" );
	_dirTreeModel->tree()->sendFinished();

	QMessageBox errorPopup( QMessageBox::Warning,	// icon
				tr( "Error" ),		// title
				tr( "Could not open directory %1" ).arg( ex.resourceName() ), // text
				QMessageBox::Ok,	// buttons
				this );			// parent
	errorPopup.setDetailedText( ex.what() );
	errorPopup.exec();
	askOpenDir();
    }

    updateActions();
    expandTreeToLevel( 1 );
}


void MainWindow::askOpenDir()
{
    QString path;
    DirTree * tree = _dirTreeModel->tree();
    bool crossFilesystems = tree->crossFilesystems();

#if USE_CUSTOM_OPEN_DIR_DIALOG
    path = QDirStat::OpenDirDialog::askOpenDir( &crossFilesystems, this );
#else
    path = QFileDialog::getExistingDirectory( this, // parent
                                              tr("Select directory to scan") );
#endif

    if ( ! path.isEmpty() )
    {
	tree->reset();
	tree->setCrossFilesystems( crossFilesystems );
	openUrl( path );
    }
}


void MainWindow::askOpenPkg()
{
    bool canceled;
    PkgFilter pkgFilter = OpenPkgDialog::askPkgFilter( canceled );

    if ( ! canceled )
    {
	_dirTreeModel->tree()->reset();
	readPkg( pkgFilter );
    }
}


void MainWindow::askShowUnpkgFiles()
{
    PkgManager * pkgManager = PkgQuery::primaryPkgManager();

    if ( ! pkgManager )
    {
	logError() << "No supported primary package manager" << endl;
	return;
    }

    ShowUnpkgFilesDialog dialog( this );

    if ( dialog.exec() == QDialog::Accepted )
	showUnpkgFiles( dialog.values() );
}


void MainWindow::showUnpkgFiles( const QString & url )
{
    UnpkgSettings unpkgSettings( UnpkgSettings::ReadFromConfig );
    unpkgSettings.startingDir = url;

    showUnpkgFiles( unpkgSettings );
}


void MainWindow::showUnpkgFiles( const UnpkgSettings & unpkgSettings )
{
    logDebug() << "Settings:" << endl;
    unpkgSettings.dump();

    PkgManager * pkgManager = PkgQuery::primaryPkgManager();

    if ( ! pkgManager )
    {
	logError() << "No supported primary package manager" << endl;
	return;
    }

    _dirTreeModel->clear(); // For instant feedback
    BusyPopup msg( tr( "Reading file lists..." ), this );

    QString dir = unpkgSettings.startingDir;
    dir.replace( QRegExp( "^unpkg:" ), "" );

    if ( dir != unpkgSettings.startingDir )
	logInfo() << "Parsed starting dir: " << dir << endl;


    // Set up the exclude rules

    ExcludeRules * excludeRules = new ExcludeRules( unpkgSettings.excludeDirs );
    CHECK_NEW( excludeRules );

    DirTree * tree = _dirTreeModel->tree();
    tree->setExcludeRules( excludeRules );


    // Prepare the filters with the complete file list of all installed packages

    DirTreeFilter * filter = new DirTreePkgFilter( pkgManager );
    CHECK_NEW( filter );

    tree->clearFilters();
    tree->addFilter( filter );

    foreach ( const QString & pattern, unpkgSettings.ignorePatterns )
    {
	tree->addFilter( DirTreePatternFilter::create( pattern ) );
    }


    // Start reading the directory

    try
    {
	_dirTreeModel->openUrl( dir );
	updateWindowTitle( _dirTreeModel->tree()->url() );
    }
    catch ( const SysCallFailedException & ex )
    {
	CAUGHT( ex );
	updateWindowTitle( "" );
	_dirTreeModel->tree()->sendFinished();

	QMessageBox errorPopup( QMessageBox::Warning,	// icon
				tr( "Error" ),		// title
				tr( "Could not open directory %1" ).arg( ex.resourceName() ), // text
				QMessageBox::Ok,	// buttons
				this );			// parent
	errorPopup.setDetailedText( ex.what() );
	errorPopup.exec();
    }

    updateActions();
}


bool MainWindow::isUnpkgUrl( const QString & url )
{
    return url.startsWith( "unpkg:/" );
}


void MainWindow::refreshAll()
{
    _enableDirPermissionsWarning = true;
    QString url = _dirTreeModel->tree()->url();

    if ( ! url.isEmpty() )
    {
	logDebug() << "Refreshing " << url << endl;

	if ( PkgFilter::isPkgUrl( url ) )
	    _dirTreeModel->readPkg( url );
	else
	    _dirTreeModel->openUrl( url );

	updateActions();
    }
    else
    {
	askOpenDir();
    }
}


void MainWindow::refreshSelected()
{
    busyDisplay();
    _futureSelection.set( _selectionModel->selectedItems().first() );
    // logDebug() << "Setting future selection: " << _futureSelection.subtree() << endl;
    _dirTreeModel->refreshSelected();
    updateActions();
}


void MainWindow::applyFutureSelection()
{
    FileInfo * sel = _futureSelection.subtree();
    // logDebug() << "Using future selection: " << sel << endl;

    if ( sel )
    {
        _treeExpandTimer.stop();
        _futureSelection.clear();
        _selectionModel->setCurrentBranch( sel );

        if ( sel->isMountPoint() )
            _ui->dirTreeView->setExpanded( sel, true );
    }
}


void MainWindow::stopReading()
{
    if ( _dirTreeModel->tree()->isBusy() )
    {
	_dirTreeModel->tree()->abortReading();
	_ui->statusBar->showMessage( tr( "Reading aborted." ), LONG_MESSAGE );
    }
}


void MainWindow::readCache( const QString & cacheFileName )
{
    _dirTreeModel->clear();
    _history->clear();

    if ( ! cacheFileName.isEmpty() )
	_dirTreeModel->tree()->readCache( cacheFileName );
}


void MainWindow::askReadCache()
{
    QString fileName = QFileDialog::getOpenFileName( this, // parent
						     tr( "Select QDirStat cache file" ),
						     DEFAULT_CACHE_NAME );
    if ( ! fileName.isEmpty() )
	readCache( fileName );

    updateActions();
}


void MainWindow::askWriteCache()
{
    QString fileName = QFileDialog::getSaveFileName( this, // parent
						     tr( "Enter name for QDirStat cache file"),
						     DEFAULT_CACHE_NAME );
    if ( ! fileName.isEmpty() )
    {
	bool ok = _dirTreeModel->tree()->writeCache( fileName );

	if ( ok )
	{
	    showProgress( tr( "Directory tree written to file %1" ).arg( fileName ) );
	}
	else
	{
	    QMessageBox::critical( this,
				   tr( "Error" ), // Title
				   tr( "ERROR writing cache file %1").arg( fileName ) );
	}
    }
}


void MainWindow::readPkg( const PkgFilter & pkgFilter )
{
    // logInfo() << "URL: " << pkgFilter.url() << endl;

    updateWindowTitle( pkgFilter.url() );
    expandTreeToLevel( 0 );   // Performance boost: Down from 25 to 6 sec.
    _dirTreeModel->readPkg( pkgFilter );
}


void MainWindow::updateWindowTitle( const QString & url )
{
    QString windowTitle = "QDirStat";

    if ( SysUtil::runningAsRoot() )
	windowTitle += tr( " [root]" );

    if ( _urlInWindowTitle )
	windowTitle += " " + url;

    setWindowTitle( windowTitle );
}


void MainWindow::expandTreeToLevel( int level )
{
    logDebug() << "Expanding tree to level " << level << endl;

    if ( level < 1 )
	_ui->dirTreeView->collapseAll();
    else
	_ui->dirTreeView->expandToDepth( level - 1 );
}


void MainWindow::showProgress( const QString & text )
{
    _ui->statusBar->showMessage( text, _statusBarTimeout );
}


void MainWindow::showCurrent( FileInfo * item )
{
    if ( item )
    {
	QString msg = QString( "%1  (%2%3)" )
	    .arg( item->debugUrl() )
	    .arg( item->sizePrefix() )
	    .arg( formatSize( item->totalSize() ) );

	if ( item->readState() == DirPermissionDenied )
	    msg += tr( "  [Permission Denied]" );
        else if ( item->readState() == DirError )
	    msg += tr( "  [Read Error]" );

	_ui->statusBar->showMessage( msg );
    }
    else
    {
	_ui->statusBar->clearMessage();
    }
}


void MainWindow::showSummary()
{
    FileInfoSet sel = _selectionModel->selectedItems();
    int count = sel.size();

    if ( count <= 1 )
	showCurrent( _selectionModel->currentItem() );
    else
    {
	sel = sel.normalized();

	_ui->statusBar->showMessage( tr( "%1 items selected (%2 total)" )
				     .arg( count )
				     .arg( formatSize( sel.totalSize() ) ) );
    }
}


void MainWindow::updateFileDetailsView()
{
    if ( _ui->fileDetailsView->isVisible() )
    {
	FileInfoSet sel = _selectionModel->selectedItems();

	if ( sel.isEmpty() )
	    _ui->fileDetailsView->showDetails( _selectionModel->currentItem() );
	else
	{
	    if ( sel.count() == 1 )
		_ui->fileDetailsView->showDetails( sel.first() );
	    else
		_ui->fileDetailsView->showDetails( sel );
	}
    }
}


void MainWindow::startingCleanup( const QString & cleanupName )
{
    showProgress( tr( "Starting cleanup action %1" ).arg( cleanupName ) );
}


void MainWindow::cleanupFinished( int errorCount )
{
    logDebug() << "Error count: " << errorCount << endl;

    if ( errorCount == 0 )
	showProgress( tr( "Cleanup action finished successfully." ) );
    else
	showProgress( tr( "Cleanup action finished with %1 errors." ).arg( errorCount ) );
}


void MainWindow::notImplemented()
{
    QMessageBox::warning( this, tr( "Error" ), tr( "Not implemented!" ) );
}


void MainWindow::copyCurrentPathToClipboard()
{
    FileInfo * currentItem = _selectionModel->currentItem();

    if ( currentItem )
    {
	QClipboard * clipboard = QApplication::clipboard();
	QString path = currentItem->path();
	clipboard->setText( path );
	showProgress( tr( "Copied to system clipboard: %1" ).arg( path ) );
    }
    else
    {
	showProgress( tr( "No current item" ) );
    }
}


void MainWindow::navigateUp()
{
    FileInfo * currentItem = _selectionModel->currentItem();

    if ( currentItem && currentItem->parent() &&
	 currentItem->parent() != _dirTreeModel->tree()->root() )
    {
	_selectionModel->setCurrentItem( currentItem->parent(),
					 true ); // select
    }
}


void MainWindow::navigateToToplevel()
{
    FileInfo * toplevel = _dirTreeModel->tree()->firstToplevel();

    if ( toplevel )
    {
	expandTreeToLevel( 1 );
	_selectionModel->setCurrentItem( toplevel,
					 true ); // select
    }
}


void MainWindow::updateHistoryActions()
{
    _ui->actionGoBack->setEnabled   ( _history->canGoBack()    );
    _ui->actionGoForward->setEnabled( _history->canGoForward() );
}


void MainWindow::historyGoBack()
{
    navigateToUrl( _history->goBack() );
    updateHistoryActions();
}


void MainWindow::historyGoForward()
{
    navigateToUrl( _history->goForward() );
    updateHistoryActions();
}


void MainWindow::navigateToUrl( const QString & url )
{
    logDebug() << "Navigating to " << url << endl;

    if ( ! url.isEmpty() )
    {
        FileInfo * sel =
            _dirTreeModel->tree()->locate( url,
                                           true ); // findPseudoDirs

        if ( sel )

        {
            _selectionModel->setCurrentItem( sel,
                                             true ); // select
        }
    }
}


void MainWindow::addToHistory( FileInfo * item )
{
    if ( item && ! item->isDirInfo() && item->parent() )
        item = item->parent();

    if ( item )
    {
        QString url = item->debugUrl();

        if ( url != _history->currentItem() )
        {
            _history->add( url );
            updateHistoryActions();
        }
    }
}


void MainWindow::initHistoryButtons()
{
    _historyMenu = new QMenu( this );
    _historyMenu->addAction( "Dummy 1" );

    connect( _historyMenu, SIGNAL( aboutToShow()       ),
             this,         SLOT  ( updateHistoryMenu() ) );

    connect( _historyMenu, SIGNAL( triggered        ( QAction * ) ),
             this,         SLOT  ( historyMenuAction( QAction * ) ) );

    addHistoryMenuToButton( _ui->actionGoBack    );
    addHistoryMenuToButton( _ui->actionGoForward );
}


void MainWindow::addHistoryMenuToButton( QAction * action )
{
    CHECK_PTR( action );

    QWidget * widget = _ui->toolBar->widgetForAction( action );

    if ( widget )
    {
        QToolButton * toolButton = qobject_cast<QToolButton *>( widget );

        if ( toolButton )
            toolButton->setMenu( _historyMenu );
    }
}


void MainWindow::updateHistoryMenu()
{
    _historyMenu->clear();
    QActionGroup * actionGroup = new QActionGroup( _historyMenu );

    QStringList items = _history->allItems();
    int current = _history->currentIndex();

    for ( int i = items.size() - 1; i >= 0; i-- )
    {
        QAction * action = new QAction( items.at( i ), _historyMenu );
        action->setCheckable( true );
        action->setChecked( i == current );
        action->setData( i );
        actionGroup->addAction( action );
        _historyMenu->addAction( action );
    }
}


void MainWindow::historyMenuAction( QAction * action )
{
    if ( action )
    {
        QVariant data = action->data();
        int index = data.toInt();

        if ( _history->setCurrentIndex( index ) )
            navigateToUrl( _history->currentItem() );
    }
}


void MainWindow::moveToTrash()
{
    FileInfoSet selectedItems = _selectionModel->selectedItems().normalized();

    // Prepare output window

    OutputWindow * outputWindow = new OutputWindow( qApp->activeWindow() );
    CHECK_NEW( outputWindow );

    // Prepare refresher

    FileInfoSet refreshSet = Refresher::parents( selectedItems );
    _selectionModel->prepareRefresh( refreshSet );
    Refresher * refresher  = new Refresher( refreshSet, this );
    CHECK_NEW( refresher );

    connect( outputWindow, SIGNAL( lastProcessFinished( int ) ),
	     refresher,	   SLOT	 ( refresh()		      ) );

    outputWindow->showAfterTimeout();

    // Move all selected items to trash

    foreach ( FileInfo * item, selectedItems )
    {
	bool success = Trash::trash( item->path() );

	if ( success )
	    outputWindow->addStdout( tr( "Moved to trash: %1" ).arg( item->path() ) );
	else
	    outputWindow->addStderr( tr( "Move to trash failed for %1" ).arg( item->path() ) );
    }

    outputWindow->noMoreProcesses();
}


void MainWindow::openConfigDialog()
{
    if ( _configDialog && _configDialog->isVisible() )
	return;

    // For whatever crazy reason it is considerably faster to delete that
    // complex dialog and recreate it from scratch than to simply leave it
    // alive and just show it again. Well, whatever - so be it.
    //
    // And yes, I added debug logging here, in the dialog's setup(), in
    // showEvent(); I added update(). No result whatsoever.
    // Okay, then let's take the long way around.

    if ( _configDialog )
	delete _configDialog;

    _configDialog = new ConfigDialog( this );
    CHECK_PTR( _configDialog );
    _configDialog->cleanupConfigPage()->setCleanupCollection( _cleanupCollection );

    if ( ! _configDialog->isVisible() )
    {
	_configDialog->setup();
	_configDialog->show();
    }
}


void MainWindow::showElapsedTime()
{
    showProgress( tr( "Reading... %1" )
		  .arg( formatMillisecTime( _stopWatch.elapsed(), false ) ) );
}


void MainWindow::showFileTypeStats()
{
    if ( ! _fileTypeStatsWindow )
    {
	// This deletes itself when the user closes it. The associated QPointer
	// keeps track of that and sets the pointer to 0 when it happens.

	_fileTypeStatsWindow = new FileTypeStatsWindow( _selectionModel, this );
    }

    _fileTypeStatsWindow->populate( selectedDirOrRoot() );
    _fileTypeStatsWindow->show();
}


void MainWindow::showFileSizeStats()
{
    FileInfo * sel = selectedDirOrRoot();

    if ( ! sel || ! sel->hasChildren() )
	return;

    FileSizeStatsWindow::populateSharedInstance( sel );
}


void MainWindow::showFileAgeStats()
{
    if ( ! _fileAgeStatsWindow )
    {
	// This deletes itself when the user closes it. The associated QPointer
	// keeps track of that and sets the pointer to 0 when it happens.

	_fileAgeStatsWindow = new FileAgeStatsWindow( this );

        connect( _selectionModel,	SIGNAL( currentItemChanged( FileInfo *, FileInfo * ) ),
                 _fileAgeStatsWindow,   SLOT  ( syncedPopulate    ( FileInfo *		   ) ) );

        connect( _fileAgeStatsWindow,   SIGNAL( locateFilesFromYear  ( QString, short ) ),
                 this,                  SLOT  ( discoverFilesFromYear( QString, short ) ) );
    }

    _fileAgeStatsWindow->populate( selectedDirOrRoot() );
    _fileAgeStatsWindow->show();
}


void MainWindow::showFilesystems()
{
    if ( ! _filesystemsWindow )
    {
	// This deletes itself when the user closes it. The associated QPointer
	// keeps track of that and sets the pointer to 0 when it happens.

	_filesystemsWindow = new FilesystemsWindow( this );

        connect( _filesystemsWindow, SIGNAL( readFilesystem( QString ) ),
                 this,               SLOT  ( openUrl       ( QString ) ) );
    }

    _filesystemsWindow->populate();
    _filesystemsWindow->show();
}


void MainWindow::discoverLargestFiles()
{
    discoverFiles( new QDirStat::LargestFilesTreeWalker(),
                   tr( "Largest Files in %1" ) );
    _locateFilesWindow->sortByColumn( LocateListSizeCol, Qt::DescendingOrder );
}


void MainWindow::discoverNewestFiles()
{
    discoverFiles( new QDirStat::NewFilesTreeWalker(),
                   tr( "Newest Files in %1" ) );
    _locateFilesWindow->sortByColumn( LocateListMTimeCol, Qt::DescendingOrder );
}


void MainWindow::discoverOldestFiles()
{
    discoverFiles( new QDirStat::OldFilesTreeWalker(),
                   tr( "Oldest Files in %1" ) );
    _locateFilesWindow->sortByColumn( LocateListMTimeCol, Qt::AscendingOrder );
}


void MainWindow::discoverHardLinkedFiles()
{
    discoverFiles( new QDirStat::HardLinkedFilesTreeWalker(),
                   tr( "Files with Multiple Hard Links in %1" ) );
    _locateFilesWindow->sortByColumn( LocateListPathCol, Qt::AscendingOrder );
}


void MainWindow::discoverBrokenSymLinks()
{
    BusyPopup msg( tr( "Checking symlinks..." ), _ui->treemapView );
    discoverFiles( new QDirStat::BrokenSymLinksTreeWalker(),
                   tr( "Broken Symbolic Links in %1" ) );
    _locateFilesWindow->sortByColumn( LocateListPathCol, Qt::AscendingOrder );
}


void MainWindow::discoverSparseFiles()
{
    discoverFiles( new QDirStat::SparseFilesTreeWalker(),
                   tr( "Sparse Files in %1" ) );
    _locateFilesWindow->sortByColumn( LocateListSizeCol, Qt::DescendingOrder );
}


void MainWindow::discoverFilesFromYear( const QString & path, short year )
{
    QString headingText = tr( "Files from %1 in %2" ).arg( year ).arg( "%1");

    discoverFiles( new QDirStat::FilesFromYearTreeWalker( year ), headingText, path );
    _locateFilesWindow->sortByColumn( LocateListMTimeCol, Qt::AscendingOrder );
}


void MainWindow::discoverFiles( TreeWalker *    treeWalker,
                                const QString & headingText,
                                const QString & path )
{
    if ( ! _locateFilesWindow )
    {
	// This deletes itself when the user closes it. The associated QPointer
	// keeps track of that and sets the pointer to 0 when it happens.

	_locateFilesWindow = new LocateFilesWindow( treeWalker,
                                                    _selectionModel,
                                                    _cleanupCollection,
                                                    this );
    }
    else
    {
        _locateFilesWindow->setTreeWalker( treeWalker );
    }

    FileInfo * sel = 0;

    if ( ! path.isEmpty() )
    {
        sel = _dirTreeModel->tree()->locate( path,
                                             true ); // findPseudoDirs
    }

    if ( ! sel )
        sel = selectedDirOrRoot();

    if ( sel )
    {
        if ( ! headingText.isEmpty() )
            _locateFilesWindow->setHeading( headingText.arg( sel->url() ) );

        _locateFilesWindow->populate( sel );
        _locateFilesWindow->show();
    }
}


void MainWindow::initLayoutActions()
{
    // Qt Designer does not support QActionGroups; it was there for Qt 3, but
    // they dropped that feature for Qt 4/5.

    _layoutActionGroup = new QActionGroup( this );
    CHECK_NEW( _layoutActionGroup );

    _layoutActionGroup->addAction( _ui->actionLayout1 );
    _layoutActionGroup->addAction( _ui->actionLayout2 );
    _layoutActionGroup->addAction( _ui->actionLayout3 );

    _ui->actionLayout1->setData( "L1" );
    _ui->actionLayout2->setData( "L2" );
    _ui->actionLayout3->setData( "L3" );
}


void MainWindow::createLayouts()
{
    TreeLayout * layout;

    layout = new TreeLayout( "L1" );
    CHECK_NEW( layout );
    _layouts[ "L1" ] = layout;

    layout = new TreeLayout( "L2" );
    CHECK_NEW( layout );
    _layouts[ "L2" ] = layout;

    layout = new TreeLayout( "L3" );
    CHECK_NEW( layout );
    _layouts[ "L3" ] = layout;

    layout->showDetailsPanel = false;
}


void MainWindow::changeLayout( const QString & name )
{
    _layoutName = name;

    if ( _layoutName.isEmpty() )
    {
	// Get the layout to use from data() from the QAction that sent the signal.

	QAction * action   = qobject_cast<QAction *>( sender() );
	_layoutName = action && action->data().isValid() ?
	    action->data().toString() : "L2";
    }

    logDebug() << "Changing to layout " << _layoutName << endl;

    _ui->dirTreeView->headerTweaker()->changeLayout( _layoutName );

    if ( _currentLayout )
	saveLayout( _currentLayout );

    if ( _layouts.contains( _layoutName ) )
    {
	_currentLayout = _layouts[ _layoutName ];
	applyLayout( _currentLayout );
    }
    else
    {
	logError() << "No layout " << _layoutName << endl;
    }
}


void MainWindow::saveLayout( TreeLayout * layout )
{
    CHECK_PTR( layout );

    layout->showCurrentPath  = _ui->actionShowCurrentPath->isChecked();
    layout->showDetailsPanel = _ui->actionShowDetailsPanel->isChecked();
}


void MainWindow::applyLayout( TreeLayout * layout )
{
    CHECK_PTR( layout );

    _ui->actionShowCurrentPath->setChecked ( layout->showCurrentPath  );
    _ui->actionShowDetailsPanel->setChecked( layout->showDetailsPanel );
}


FileInfo * MainWindow::selectedDirOrRoot() const
{
    FileInfoSet selectedItems = _selectionModel->selectedItems();
    FileInfo * sel = selectedItems.first();

    if ( ! sel || ! sel->isDir() )
	sel = _dirTreeModel->tree()->firstToplevel();

    return sel;
}


void MainWindow::showDirPermissionsWarning()
{
    if ( _dirPermissionsWarning || ! _enableDirPermissionsWarning )
	return;

    PanelMessage * msg = new PanelMessage( _ui->messagePanel );
    CHECK_NEW( msg );

    msg->setHeading( tr( "Some directories could not be read." ) );
    msg->setText( tr( "You might not have sufficient permissions." ) );
    msg->setIcon( QPixmap( ":/icons/lock-closed.png" ) );

    msg->connectDetailsLink( this, SLOT( showUnreadableDirs() ) );

    _ui->messagePanel->add( msg );
    _dirPermissionsWarning = msg;
    _enableDirPermissionsWarning = false;
}


void MainWindow::showUnreadableDirs()
{
    if ( ! _unreadableDirsWindow )
    {
	// This deletes itself when the user closes it. The associated QPointer
	// keeps track of that and sets the pointer to 0 when it happens.

	_unreadableDirsWindow = new UnreadableDirsWindow( _selectionModel, this );
    }

    _unreadableDirsWindow->populate( _dirTreeModel->tree()->root() );
    _unreadableDirsWindow->show();
}


void MainWindow::toggleVerboseSelection()
{
    // Verbose selection is toggled with Shift-F7

    _verboseSelection = _ui->actionVerboseSelection->isChecked();

    if ( _selectionModel )
	_selectionModel->setVerbose( _verboseSelection );

    logInfo() << "Verbose selection is now " << ( _verboseSelection ? "on" : "off" )
	      << ". Change this with Shift-F7." << endl;
}


void MainWindow::openActionUrl()
{
    QAction * action = qobject_cast<QAction *>( sender() );

    if ( action )
    {
        QString url = action->statusTip();

        if ( url.isEmpty() )
            logError() << "No URL in statusTip() for action " << action->objectName();
        else
            SysUtil::openInBrowser( url );
    }
}


void MainWindow::showAboutDialog()
{
    QString homePage = "https://github.com/shundhammer/qdirstat";
    QString mailTo   = "qdirstat@gmx.de";

    QString text = "<h2>QDirStat " QDIRSTAT_VERSION "</h2>";
    text += "<p>";
    text += tr( "Qt-based directory statistics -- showing where all your disk space has gone "
		" and trying to help you to clean it up." );
    text += "</p><p>";
    text += "(c) 2015-2021 Stefan Hundhammer";
    text += "</p><p>";
    text += tr( "Contact: " ) + QString( "<a href=\"mailto:%1\">%2</a>" ).arg( mailTo ).arg( mailTo );
    text += "</p><p>";
    text += QString( "<p><a href=\"%1\">%2</a></p>" ).arg( homePage ).arg( homePage );
    text += tr( "License: GPL V2 (GNU General Public License Version 2)" );
    text += "</p><p>";
    text += tr( "This is free Open Source software, provided to you hoping that it might be "
		"useful for you. It does not cost you anything, but on the other hand there "
		"is no warranty or promise of anything." );
    text += "</p><p>";
    text += tr( "This software was made with the best intentions and greatest care, but still "
		"there is the off chance that something might go wrong which might damage "
		"data on your computer. Under no circumstances will the authors of this program "
		"be held responsible for anything like that. Use this program at your own risk." );
    text += "</p>";

    QMessageBox::about( this, tr( "About QDirStat" ), text );
}


void MainWindow::showDonateDialog()
{
    QString dUrl = "https://www.paypal.com/cgi-bin/webscr?cmd=_s-xclick&hosted_button_id=EYJXAVLGNRR5W";

    QString text = "<h2>Donate</h2>";
    text += "<p>";
    text += tr( "QDirStat is Free Open Source Software. "
		"You are not required to pay anything. "
		"Donations are most welcome, of course." );
    text += "</p><p>";
    text += tr( "You can donate any amount of your choice:" );
    text += "</p><p>";
    text += QString( "<a href=\"%1\">QDirStat at PayPal</a>" ).arg(_dUrl );
    text += "</p><p>";
    text += tr( "(external browser window)" );
    text += "</p>";

    QMessageBox::about( this, tr( "Donate" ), text );
}




//---------------------------------------------------------------------------
//			       Debugging Helpers
//---------------------------------------------------------------------------


void MainWindow::itemClicked( const QModelIndex & index )
{
    if ( ! _verboseSelection )
	return;

    if ( index.isValid() )
    {
	FileInfo * item = static_cast<FileInfo *>( index.internalPointer() );

	logDebug() << "Clicked row " << index.row()
		   << " col " << index.column()
		   << " (" << QDirStat::DataColumns::fromViewCol( index.column() ) << ")"
		   << "\t" << item
		   << endl;
	// << " data(0): " << index.model()->data( index, 0 ).toString()
	// logDebug() << "Ancestors: " << Debug::modelTreeAncestors( index ).join( " -> " ) << endl;
    }
    else
    {
	logDebug() << "Invalid model index" << endl;
    }

    // _dirTreeModel->dumpPersistentIndexList();
}


void MainWindow::selectionChanged()
{
    showSummary();
    updateFileDetailsView();

    if ( _verboseSelection )
    {
	logDebug() << endl;
	_selectionModel->dumpSelectedItems();
    }
}


void MainWindow::currentItemChanged( FileInfo * newCurrent, FileInfo * oldCurrent )
{
    showSummary();

    if ( ! oldCurrent )
	updateFileDetailsView();

    if ( _verboseSelection )
    {
	logDebug() << "new current: " << newCurrent << endl;
	logDebug() << "old current: " << oldCurrent << endl;
	_selectionModel->dumpSelectedItems();
    }
}


QString MainWindow::formatMillisecTime( qint64 millisec, bool showMillisec )
{
    QString formattedTime;
    int hours;
    int min;
    int sec;

    hours	= millisec / 3600000L;	// 60*60*1000
    millisec	%= 3600000L;

    min		= millisec / 60000L;	// 60*1000
    millisec	%= 60000L;

    sec		= millisec / 1000L;
    millisec	%= 1000L;

    if ( hours < 1 && min < 1 && sec < 60 )
    {
	formattedTime.setNum( sec );

	if ( showMillisec )
	{
	    formattedTime += QString( ".%1" ).arg( millisec,
						   3,	// fieldWidth
						   10,	// base
						   QChar( '0' ) ); // fillChar
	}

	formattedTime += " " + tr( "sec" );
    }
    else
    {
	formattedTime = QString( "%1:%2:%3" )
	    .arg( hours, 2, 10, QChar( '0' ) )
	    .arg( min,	 2, 10, QChar( '0' ) )
	    .arg( sec,	 2, 10, QChar( '0' ) );
    }

    return formattedTime;
}
