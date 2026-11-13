/*
 * Copyright (C) 2026 LogSquirl Contributors
 *
 * This file is part of logsquirl-serial.
 *
 * logsquirl-serial is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * logsquirl-serial is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with logsquirl-serial.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file plugin.cpp
 * @brief C ABI entry points for the LogSquirl serial monitor plugin.
 *
 * This file implements the exported symbols that every LogSquirl plugin
 * must provide:
 *
 *   - logsquirl_plugin_get_info()   → static metadata
 *   - logsquirl_plugin_init_ex()    → create the plugin instance, for
 *                                     LogSquirl 26.11 and later, with the
 *                                     size of the host's table
 *   - logsquirl_plugin_init()       → init_ex() for an older host
 *   - logsquirl_plugin_shutdown()   → destroy the plugin instance
 *   - logsquirl_plugin_configure()  → open default settings dialog
 *
 * PLUGIN LIFECYCLE
 * ────────────────
 *   1. Host calls get_info() to read metadata.
 *   2. Host calls init_ex(api, handle, api_size) — or init(api, handle),
 *      which is init_ex() with the base table's size, if it is older than
 *      LogSquirl 26.11 — which creates the Plugin: the Host over the table,
 *      the ProfileStore of the saved port configurations, the Sessions that
 *      capture, the PortDialog that edits the configurations and the
 *      SidebarWidget with a tile for each, registered as a sidebar tab; and
 *      a menu action that opens the dialog.
 *   3. User saves configurations in the dialog, and starts and stops their
 *      captures from their tiles.
 *   4. Host calls shutdown(), which destroys the Plugin: it stops the
 *      captures, and its parts go in the reverse order of their
 *      declaration, each before the parts it uses.
 *
 * No exception may leave an entry point or a host callback: the host is C,
 * and an exception escaping into it terminates LogSquirl.  Their work runs
 * through guarded(), which logs the failure instead.
 *
 * @see logsquirl_plugin_api.h for the full host API reference.
 */

#include "plugin.h"

#include "portdialog.h"
#include "profiles.h"
#include "serialprocess.h"
#include "sessions.h"
#include "sidebarwidget.h"
#include "tempdirs.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPointer>
#include <QSpinBox>
#include <QWindow>

#include <exception>

namespace serial_monitor {
namespace {

/**
 * A widget the plugin owns and the host parents (logsquirl_plugin_api.h):
 * deleted with its owner, unless it went with its parent already.
 */
template <typename W>
class OwnedWidget {
public:
    template <typename... Args>
    explicit OwnedWidget( Args&&... args )
        : widget_( new W( std::forward<Args>( args )... ) )
    {
    }

    ~OwnedWidget()
    {
        delete widget_.data();
    }

    OwnedWidget( const OwnedWidget& ) = delete;
    OwnedWidget& operator=( const OwnedWidget& ) = delete;

    W* get() const
    {
        return widget_.data();
    }

private:
    QPointer<W> widget_;
};

/**
 * A sidebar tab, registered for its lifetime: the host takes @p widget as
 * a tab on construction, and lets go of it on destruction, so before the
 * widget goes if the tab is declared after it.  A host that throws on
 * registering leaves nothing to unregister.
 */
class SidebarTab {
public:
    SidebarTab( const LogSquirlHostApi* api, void* handle, Host& host, QWidget* widget )
        : api_( api )
        , handle_( handle )
        , host_( host )
        , widget_( widget )
    {
        api_->register_sidebar_tab( handle_, "Serial", widget_ );
    }

    ~SidebarTab()
    {
        guarded( host_, "unregistering the sidebar tab",
                 [ this ] { api_->unregister_sidebar_tab( handle_, widget_ ); } );
    }

    SidebarTab( const SidebarTab& ) = delete;
    SidebarTab& operator=( const SidebarTab& ) = delete;

private:
    const LogSquirlHostApi* api_;
    void* handle_;
    Host& host_;
    void* widget_;
};

/**
 * The temporary directories in the host's temporary root (tempdirs.h):
 * those of LogSquirl processes that ended, e.g. after a crash, go on
 * construction, as no tab can show them any more; this process's on
 * destruction if LogSquirl quits, as their tabs close with it.
 */
class TemporaryFiles {
public:
    explicit TemporaryFiles( Host& host )
        : host_( host )
    {
        removeStaleTempDirs( host_.tempRoot() );
    }

    /// Remove the files of every instance of the plugin in this process,
    /// also those of instances before a runtime disable or update, which
    /// only the directory names remember.  Save paths and the log
    /// directory are never touched.
    ~TemporaryFiles()
    {
        if ( quitting_ ) {
            guarded( host_, "removing temporary files",
                     [ this ] { removeOwnTempDirs( host_.tempRoot() ); } );
        }
    }

    TemporaryFiles( const TemporaryFiles& ) = delete;
    TemporaryFiles& operator=( const TemporaryFiles& ) = delete;

    /// LogSquirl quits (aboutToQuit), rather than unloading the plugin with
    /// the tabs left open: the files may go on destruction.
    void setQuitting()
    {
        quitting_ = true;
    }

private:
    Host& host_;
    bool quitting_ = false;
};

/**
 * The plugin between init() and shutdown(): every part, owned.  The
 * members are declared in the order they depend on each other, so that
 * destroying it destroys each part before those it uses; the destructor
 * stops the captures first, while every part that shows them is there.
 */
class Plugin {
public:
    /// Set up every part, and register the sidebar tab and the menu entry
    /// with the host's table @p api (of @p apiSize bytes) for @p handle.  An
    /// exception leaves nothing behind: the parts made go again, in reverse.
    Plugin( const LogSquirlHostApi* api, void* handle, std::size_t apiSize,
            const QString& tempRoot );

    ~Plugin();

    Plugin( const Plugin& ) = delete;
    Plugin& operator=( const Plugin& ) = delete;

    Host& host()
    {
        return host_;
    }

    std::size_t hostApiSize() const
    {
        return hostApiSize_;
    }

    /// Show the configuration dialog, on top of the window it was opened from.
    void showDialog();

    /// Let the user change the defaults (baud rate and timestamps of a new
    /// configuration, the size at which a log file is rotated) and save them.
    void configureDefaults( QWidget* parent );

private:
    CApiHost host_;           ///< What every part tells LogSquirl through.
    std::size_t hostApiSize_; ///< The size of the host's table (init_ex()).
    TemporaryFiles temporaryFiles_;
    ProfileStore profiles_;              ///< serial.ini: defaults, configurations.
    Sessions sessions_;                  ///< The running captures, which the views show.
    PortDialog dialog_;                  ///< Edits a saved configuration.
    OwnedWidget<SidebarWidget> sidebar_; ///< One tile per configuration.
    SidebarTab sidebarTab_;              ///< sidebar_, as the host's tab.
};

/// The running plugin, between init() and shutdown(); the only global.
/// Not a smart pointer: one destroyed with the library would destroy the
/// Qt objects after the application, if the host never called shutdown().
Plugin* g_plugin = nullptr;

/// Where the next init() puts the temporary directories; for the tests.
QString& tempRootForTests()
{
    static QString tempRoot;
    return tempRoot;
}

/// Called when the user clicks "Serial Monitor…" in the Plugins menu: the
/// dialog as it was left.
void showSerialDialog( void* /* userData */ )
{
    guarded( "showing the Serial Monitor dialog", [] {
        if ( g_plugin ) {
            g_plugin->showDialog();
        }
    } );
}

Plugin::Plugin( const LogSquirlHostApi* api, void* handle, std::size_t apiSize,
                const QString& tempRoot )
    : host_( api, handle, tempRoot )
    , hostApiSize_( apiSize )
    , temporaryFiles_( host_ )
    , profiles_( host_.configDir(),
                 [ this ]( const QString& name ) { return sessions_.isRunning( name ); } )
    // Scans the ports, once, for every view.
    , sessions_( host_ )
    // Parentless, so that it never goes with a main window that is
    // destroyed first; showDialog() keeps it on top of one.
    , dialog_( &profiles_, &sessions_ )
    , sidebar_( host_, &profiles_, &sessions_ )
    , sidebarTab_( api, handle, host_, sidebar_.get() )
{
    // Add "Serial Monitor…" to the Plugins menu.  When clicked it opens the
    // non-modal dialog that edits the saved port configurations.
    api->register_menu_action( handle, "Plugins", "Serial Monitor…", &showSerialDialog, nullptr );

    // The sidebar asks for the dialog: for a new configuration, or a tile's.
    QObject::connect( sidebar_.get(), &SidebarWidget::newConfigurationRequested, &dialog_,
                      [ this ] {
                          guarded( host_, "opening a new serial configuration", [ this ] {
                              dialog_.newConfiguration();
                              showDialog();
                          } );
                      } );
    QObject::connect( sidebar_.get(), &SidebarWidget::editRequested, &dialog_,
                      [ this ]( const QString& name ) {
                          guarded( host_, "editing a serial configuration", [ this, &name ] {
                              dialog_.editConfiguration( name );
                              showDialog();
                          } );
                      } );

    // The host shuts the plugin down both when LogSquirl quits (after
    // aboutToQuit) and when the plugin is disabled or updated at runtime,
    // with the tabs left open; only in the first case may the temporary
    // files go.
    if ( auto* app = QCoreApplication::instance() ) {
        QObject::connect( app, &QCoreApplication::aboutToQuit, &sessions_,
                          [ this ] { temporaryFiles_.setQuitting(); } );
    }

    host_.log( LOGSQUIRL_LOG_INFO, "Serial Monitor plugin ready." );
}

Plugin::~Plugin()
{
    // Each capture keeps its file for its tab, also when the plugin is
    // disabled or updated at runtime.
    guarded( host_, "stopping the serial sessions", [ this ] { sessions_.stopAll(); } );
}

void Plugin::showDialog()
{
    // A transient parent keeps the dialog on top of the window whose menu
    // opened it, without handing over ownership.
    auto* window = QApplication::activeWindow();
    if ( window && window != &dialog_ ) {
        dialog_.winId(); // creates the native window, and so windowHandle()
        dialog_.windowHandle()->setTransientParent( window->windowHandle() );
    }

    dialog_.show();
    dialog_.raise();
    dialog_.activateWindow();
}

void Plugin::configureDefaults( QWidget* parent )
{
    // Without a configuration directory nothing can be saved.
    if ( const auto problem = profiles_.defaultsUnsavable(); !problem.isEmpty() ) {
        host_.warn( problem );
        return;
    }

    const auto defaults = profiles_.defaults();

    QDialog dialog( parent );
    dialog.setWindowTitle( "Configure Serial Monitor" );
    auto* layout = new QFormLayout( &dialog );

    auto* baudSpin = new QSpinBox( &dialog );
    baudSpin->setRange( 300, 4000000 );
    baudSpin->setValue( defaults.baudRate );
    layout->addRow( "Default baud rate:", baudSpin );

    auto* timestampCheckBox = new QCheckBox( "Prepend timestamp to each line", &dialog );
    timestampCheckBox->setChecked( defaults.timestamps );
    layout->addRow( timestampCheckBox );

    auto* maxFileSpin = new QSpinBox( &dialog );
    maxFileSpin->setObjectName( "maxFileMiB" );
    maxFileSpin->setRange( 0, 1024 * 1024 ); // up to 1 TiB
    maxFileSpin->setSuffix( " MiB" );
    maxFileSpin->setSpecialValueText( "Never" );
    maxFileSpin->setValue( defaults.maxFileMiB );
    maxFileSpin->setToolTip( "When a log file reaches this size, the capture goes on in a new "
                             "file, opened in a new tab" );
    layout->addRow( "Start a new log file at:", maxFileSpin );

    auto* buttons
        = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog );
    QObject::connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
    QObject::connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
    layout->addRow( buttons );

    if ( dialog.exec() != QDialog::Accepted ) {
        return;
    }

    Defaults chosen;
    chosen.baudRate = baudSpin->value();
    chosen.timestamps = timestampCheckBox->isChecked();
    chosen.maxFileMiB = maxFileSpin->value();
    if ( const auto problem = profiles_.saveDefaults( chosen ); !problem.isEmpty() ) {
        host_.warn( problem, LOGSQUIRL_LOG_ERROR );
        return;
    }
    host_.log( LOGSQUIRL_LOG_INFO, QString( "Default baud rate set to %1, timestamps %2" )
                                       .arg( baudSpin->value() )
                                       .arg( timestampCheckBox->isChecked() ? "on" : "off" ) );
    // A new configuration proposes them from now on (ProfileStore::newProfile());
    // the saved ones keep their own settings.
}

} // namespace

Host& pluginHost() noexcept
{
    static CApiHost detached;
    return g_plugin ? g_plugin->host() : detached;
}

std::size_t hostApiSize() noexcept
{
    return g_plugin ? g_plugin->hostApiSize() : 0;
}

void setTempRootForTests( const QString& tempRoot )
{
    tempRootForTests() = tempRoot;
}

} // namespace serial_monitor

using serial_monitor::guarded;

// ── Plugin metadata ──────────────────────────────────────────────────────

/// Static plugin info.  All strings have static storage duration.
static const LogSquirlPluginInfo kPluginInfo = {
    /* id          */ "io.github.logsquirl.serial",
    /* name        */ "Serial Monitor",
    /* version     */ LOGSQUIRL_PLUGIN_VERSION,
    /* description */ "Stream serial port data into LogSquirl tabs",
    /* author      */ "LogSquirl Contributors",
    /* license     */ "GPL-3.0-or-later",
    /* type        */ LOGSQUIRL_PLUGIN_UI,
    /* api_version */ LOGSQUIRL_PLUGIN_API_VERSION,
};

// ── Exported C entry points ──────────────────────────────────────────────

extern "C" {

/**
 * Return static plugin metadata.
 *
 * The host calls this before init() to read the plugin's identity, type,
 * and API version.  The returned pointer is valid for the lifetime of the
 * shared library.
 */
LOGSQUIRL_PLUGIN_EXPORT const LogSquirlPluginInfo* logsquirl_plugin_get_info( void )
{
    return &kPluginInfo;
}

/**
 * Initialise the plugin.
 *
 * @param api     Host API function table — valid until shutdown().
 * @param handle  Opaque handle — pass back to every host API call.
 * @param api_size  The size of the table @p api points to, in the host's
 *                  build: it tells which later functions the host has.
 * @return 0 on success, non-zero on failure.
 *
 * Creates the plugin instance, which registers a menu action that opens
 * the serial monitor dialog, and a sidebar tab.  A failure is logged, what
 * was set up goes again, and init returns 1.
 */
LOGSQUIRL_PLUGIN_EXPORT int logsquirl_plugin_init_ex( const LogSquirlHostApi* api, void* handle,
                                                      size_t api_size )
{
    using serial_monitor::g_plugin;
    if ( !api || !handle ) {
        return 1;
    }

    // Logs a failure even when the instance it would have been the host of
    // was not made.
    serial_monitor::CApiHost host( api, handle, serial_monitor::tempRootForTests() );
    serial_monitor::Plugin* created = nullptr;
    const bool ok = guarded( host, "initialisation", [ & ] {
        host.log( LOGSQUIRL_LOG_INFO, "Serial Monitor plugin initialising…" );
        created = new serial_monitor::Plugin( api, handle, api_size, host.tempRoot() );
    } );
    if ( !ok ) {
        return 1;
    }
    g_plugin = created;
    return 0;
}

/**
 * Initialise the plugin for a host older than init_ex(): its table ends
 * where the functions LogSquirl 26.11 added begin.  The host refuses a
 * plugin without this entry point.
 */
LOGSQUIRL_PLUGIN_EXPORT int logsquirl_plugin_init( const LogSquirlHostApi* api, void* handle )
{
    return logsquirl_plugin_init_ex( api, handle, LOGSQUIRL_HOST_API_BASE_SIZE );
}

/**
 * Shut down the plugin — stop all sessions and release resources.
 *
 * The host calls this before unloading the shared library.  After this
 * function returns, no host API calls may be made.
 */
LOGSQUIRL_PLUGIN_EXPORT void logsquirl_plugin_shutdown( void )
{
    using serial_monitor::g_plugin;
    if ( !g_plugin ) {
        return; // not initialised, or failed to
    }
    guarded( "shutdown", [] {
        serial_monitor::pluginHost().log( LOGSQUIRL_LOG_INFO,
                                          "Serial Monitor plugin shutting down…" );
    } );
    // Still the running plugin while it goes, so that what its parts log on
    // the way reaches the host.
    delete g_plugin;
    g_plugin = nullptr;
}

/**
 * Open a configuration dialog for default serial settings.
 *
 * @param parent_widget  Cast of a QWidget* the plugin can use as dialog parent.
 *
 * Lets the user change the default baud rate and whether lines are
 * timestamped, which a new configuration proposes, and the size at which a
 * log file is rotated, for every capture.
 */
LOGSQUIRL_PLUGIN_EXPORT void logsquirl_plugin_configure( void* parent_widget )
{
    guarded( "configuring", [ parent_widget ] {
        if ( serial_monitor::g_plugin ) {
            serial_monitor::g_plugin->configureDefaults( static_cast<QWidget*>( parent_widget ) );
        }
    } );
}

} // extern "C"
