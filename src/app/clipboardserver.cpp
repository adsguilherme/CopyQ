/*
    Copyright (c) 2017, Lukas Holecek <hluk@email.cz>

    This file is part of CopyQ.

    CopyQ is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    CopyQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with CopyQ.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "clipboardserver.h"

#include "app/remoteprocess.h"
#include "common/appconfig.h"
#include "common/clientsocket.h"
#include "common/client_server.h"
#include "common/commandstatus.h"
#include "common/display.h"
#include "common/log.h"
#include "common/mimetypes.h"
#include "common/monitormessagecode.h"
#include "common/shortcuts.h"
#include "common/sleeptimer.h"
#include "gui/clipboardbrowser.h"
#include "gui/commanddialog.h"
#include "gui/configtabshortcuts.h"
#include "gui/iconfactory.h"
#include "gui/mainwindow.h"
#include "item/itemfactory.h"
#include "item/serialize.h"
#include "scriptable/scriptableproxy.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QSessionManager>

#ifdef NO_GLOBAL_SHORTCUTS
class QxtGlobalShortcut {};
#else
#include "../qxt/qxtglobalshortcut.h"
#endif

ClipboardServer::ClipboardServer(QApplication *app, const QString &sessionName)
    : QObject()
    , App("Server", app, sessionName)
    , m_wnd(nullptr)
    , m_monitor(nullptr)
    , m_shortcutActions()
    , m_ignoreKeysTimer()
{
    const QString serverName = clipboardServerName();
    auto server = new Server(serverName, this);

    if ( server->isListening() ) {
        ::createSessionMutex();
        restoreSettings(true);
        COPYQ_LOG("Server \"" + serverName + "\" started.");
    } else {
        restoreSettings(false);
        COPYQ_LOG("Server \"" + serverName + "\" already running!");
        log( tr("CopyQ server is already running."), LogWarning );
        exit(0);
        return;
    }

#if QT_VERSION >= 0x050000
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
#endif

    QApplication::setQuitOnLastWindowClosed(false);

    m_itemFactory = new ItemFactory(this);
    m_wnd = new MainWindow(m_itemFactory);

    m_itemFactory->loadPlugins();
    if ( !m_itemFactory->hasLoaders() )
        log("No plugins loaded", LogNote);

    m_wnd->loadSettings();
    m_wnd->setCurrentTab(0);
    m_wnd->enterBrowseMode();

    connect( server, SIGNAL(newConnection(ClientSocketPtr)),
             this, SLOT(onClientNewConnection(ClientSocketPtr)) );

    connect( qApp, SIGNAL(aboutToQuit()),
             this, SLOT(onAboutToQuit()));

    connect( qApp, SIGNAL(commitDataRequest(QSessionManager&)),
             this, SLOT(onCommitData(QSessionManager&)) );

    connect( qApp, SIGNAL(saveStateRequest(QSessionManager&)),
             this, SLOT(onSaveState(QSessionManager&)) );

    connect( m_wnd, SIGNAL(changeClipboard(QVariantMap,QClipboard::Mode)),
             this, SLOT(changeClipboard(QVariantMap,QClipboard::Mode)) );

    connect( m_wnd, SIGNAL(requestExit()),
             this, SLOT(maybeQuit()) );

    loadSettings();

    // notify window if configuration changes
    connect( m_wnd, SIGNAL(configurationChanged()),
             this, SLOT(loadSettings()) );

    connect( m_wnd, SIGNAL(commandsSaved()),
             this, SLOT(onCommandsSaved()) );
    onCommandsSaved();

    // run clipboard monitor
    startMonitoring();

    qApp->installEventFilter(this);

    server->start();

    // Ignore global shortcut key presses in any widget.
    m_ignoreKeysTimer.setInterval(100);
    m_ignoreKeysTimer.setSingleShot(true);
}

ClipboardServer::~ClipboardServer()
{
    removeGlobalShortcuts();
    delete m_wnd;
}

void ClipboardServer::stopMonitoring()
{
    if (m_monitor == nullptr)
        return;

    COPYQ_LOG("Terminating monitor");

    m_monitor->disconnect();
    delete m_monitor;
    m_monitor = nullptr;

    COPYQ_LOG("Monitor terminated");
}

void ClipboardServer::startMonitoring()
{
    COPYQ_LOG("Starting monitor");

    if ( m_monitor == nullptr ) {
        m_monitor = new RemoteProcess(this);
        connect( m_monitor, SIGNAL(newMessage(QByteArray)),
                 this, SLOT(newMonitorMessage(QByteArray)) );
        connect( m_monitor, SIGNAL(connectionError(QString)),
                 this, SLOT(monitorConnectionError(QString)) );
        connect( m_monitor, SIGNAL(connected()),
                 this, SLOT(loadMonitorSettings()) );

        const QString name = serverName("m");
        const auto session = qApp->property("CopyQ_session_name").toString();
        m_monitor->start( name, QStringList() << "-s" << session << "monitor" << name );
    }
}

void ClipboardServer::loadMonitorSettings()
{
    if ( !isMonitoring() )
        return;

    COPYQ_LOG("Configuring monitor");

    QVariantMap settings;
    settings["formats"] = m_itemFactory->formatsToSave();
#ifdef HAS_MOUSE_SELECTIONS
    settings["check_selection"] = AppConfig().option<Config::check_selection>();
#endif

    QByteArray settingsData;
    QDataStream settingsOut(&settingsData, QIODevice::WriteOnly);
    settingsOut << settings;

    m_monitor->writeMessage(settingsData, MonitorSettings);
}

bool ClipboardServer::isMonitoring()
{
    return m_monitor != nullptr && m_monitor->isConnected();
}

void ClipboardServer::removeGlobalShortcuts()
{
    for (auto it = m_shortcutActions.constBegin(); it != m_shortcutActions.constEnd(); ++it)
        delete it.key();
    m_shortcutActions.clear();
}

void ClipboardServer::onCommandsSaved()
{
    removeGlobalShortcuts();

    QList<QKeySequence> usedShortcuts;

    m_scriptCommands.clear();

    const auto commands = loadEnabledCommands();
    for (const auto &command : commands) {
        const auto type = command.type();

        if (type & CommandType::Script)
            m_scriptCommands.append(command);

#ifndef NO_GLOBAL_SHORTCUTS
        if (type & CommandType::GlobalShortcut) {
            for (const auto &shortcutText : command.globalShortcuts) {
                QKeySequence shortcut(shortcutText, QKeySequence::PortableText);
                if ( !shortcut.isEmpty() && !usedShortcuts.contains(shortcut) ) {
                    usedShortcuts.append(shortcut);
                    createGlobalShortcut(shortcut, command);
                }
            }
        }
#endif
    }
}

void ClipboardServer::onAboutToQuit()
{
    COPYQ_LOG("Closing server.");

    m_wnd->saveTabs();

    if( isMonitoring() )
        stopMonitoring();

    emit terminateClients();
}

void ClipboardServer::onCommitData(QSessionManager &sessionManager)
{
    COPYQ_LOG("Got commit data request from session manager.");

    const bool cancel = sessionManager.allowsInteraction() && !askToQuit();
    sessionManager.release();

    if (cancel) {
        sessionManager.cancel();
    } else {
        m_wnd->saveTabs();

        // WORKAROUND: This is required to exit application from
        //             installer, otherwise main window is only
        //             minimized after this when tray is disabled.
        m_wnd->hide();
        exit();
    }
}

void ClipboardServer::onSaveState(QSessionManager &sessionManager)
{
    COPYQ_LOG("Got save state request from session manager.");

    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "copyq", "copyq_no_session");
    const auto sessionNameKey = "session_" + sessionManager.sessionId();
    const auto sessionName = qApp->property("CopyQ_session_name").toString();
    settings.setValue(sessionNameKey, sessionName);

    // Remove last session name from configuration.
    const auto lastSessionIdKey = "last_session_id_for_" + sessionName;
    const auto lastSessionId = settings.value(lastSessionIdKey).toString();
    if ( !lastSessionId.isEmpty() ) {
        const auto lastSessionNameKey = "session_" + lastSessionId;
        settings.remove(lastSessionNameKey);
    }
    settings.setValue(lastSessionIdKey, sessionNameKey);
}

void ClipboardServer::maybeQuit()
{
    // Wait a moment for commands to finish.
    for ( int i = 0; i < 50 && hasRunningCommands(); ++i )
        waitFor(50);

    if (askToQuit()) {
        emit terminateClients();
        QCoreApplication::exit();
    }
}

bool ClipboardServer::askToQuit()
{
    if ( !m_wnd->maybeCloseCommandDialog() )
        return false;

    if ( hasRunningCommands() ) {
        QMessageBox messageBox( QMessageBox::Warning, tr("Cancel Active Commands"),
                                tr("Cancel active commands and exit?"), QMessageBox::NoButton,
                                m_wnd );

        messageBox.addButton(tr("Cancel Exiting"), QMessageBox::RejectRole);
        messageBox.addButton(tr("Exit Anyway"), QMessageBox::AcceptRole);

        return messageBox.exec() == QMessageBox::Accepted;
    }

    return true;
}

bool ClipboardServer::hasRunningCommands() const
{
    return m_wnd->hasRunningAction() || !m_clients.isEmpty();
}

void ClipboardServer::onClientNewConnection(const ClientSocketPtr &client)
{
    auto proxy = new ScriptableProxy(m_wnd, client.get());
    m_clients.insert( client.get(), ClientData(client, proxy) );
    connect( this, SIGNAL(terminateClients()),
             client.get(), SLOT(close()) );
    connect( client.get(), SIGNAL(messageReceived(QByteArray,int,ClientSocket*)),
             this, SLOT(onClientMessageReceived(QByteArray,int,ClientSocket*)) );
    connect( client.get(), SIGNAL(disconnected(ClientSocket*)),
             this, SLOT(onClientDisconnected(ClientSocket*)) );
    connect( client.get(), SIGNAL(connectionFailed(ClientSocket*)),
             this, SLOT(onClientConnectionFailed(ClientSocket*)) );
    client->start();
}

void ClipboardServer::onClientMessageReceived(
        const QByteArray &message, int messageCode, ClientSocket *client)
{
    Q_UNUSED(client);
    switch (messageCode) {
    case CommandGetData: {
        auto proxy = m_clients.value(client).proxy;
        if (!proxy)
            return;

        QVariantMap actionData;
        if ( !message.isEmpty() ) {
            QDataStream in(message);
            int actionId;
            in >> actionId;
            Q_ASSERT(in.status() == QDataStream::Ok);
            actionData = proxy->getActionData(actionId);
        }

        QByteArray bytes;
        QDataStream out(&bytes, QIODevice::WriteOnly);
        out << m_scriptCommands << actionData;
        Q_ASSERT(out.status() == QDataStream::Ok);

        client->sendMessage(bytes, CommandSetData);
        break;
    }
    case CommandFunctionCall: {
        auto proxy = m_clients.value(client).proxy;
        if (!proxy)
            return;
        const auto result = proxy->callFunction(message);
        client->sendMessage(result, CommandFunctionCallReturnValue);
        break;
    }
    default:
        log(QString("Unhandled command status: %1").arg(messageCode));
        break;
    }
}

void ClipboardServer::onClientDisconnected(ClientSocket *client)
{
    m_clients.remove(client);
}

void ClipboardServer::onClientConnectionFailed(ClientSocket *client)
{
    log("Client connection failed", LogWarning);
    m_clients.remove(client);
}

void ClipboardServer::newMonitorMessage(const QByteArray &message)
{
    if ( !m_wnd->isMonitoringEnabled() )
        return;

    QVariantMap data;

    if ( !deserializeData(&data, message) ) {
        log("Failed to read message from monitor.", LogError);
        return;
    }

    m_wnd->clipboardChanged(data);
}

void ClipboardServer::monitorConnectionError(const QString &error)
{
    log("Restarting clipboard monitor (" + error + ")", LogError);
    stopMonitoring();
    startMonitoring();
}

void ClipboardServer::changeClipboard(const QVariantMap &data, QClipboard::Mode mode)
{
    if ( !isMonitoring() ) {
        COPYQ_LOG("Waiting for monitor to start");
        SleepTimer t(5000);
        while ( t.sleep() && !isMonitoring() ) {}
        if ( !isMonitoring() ) {
            log("Failed to send message to clipboard monitor.", LogError);
            return;
        }
    }

    const MonitorMessageCode code =
            mode == QClipboard::Clipboard ? MonitorChangeClipboard : MonitorChangeSelection;

    const auto message = serializeData(data);

    COPYQ_LOG( QString("Sending change %1 request to monitor (%2 KiB)")
               .arg(code == MonitorChangeClipboard ? "clipboard" : "selection")
               .arg(message.size() / 1024.0) );

    m_monitor->writeMessage(message, code);
}

void ClipboardServer::createGlobalShortcut(const QKeySequence &shortcut, const Command &command)
{
#ifdef NO_GLOBAL_SHORTCUTS
    Q_UNUSED(shortcut);
    Q_UNUSED(command);
#else
    auto s = new QxtGlobalShortcut(shortcut, this);
    if (!s->isValid()) {
        log(QString("Failed to set global shortcut \"%1\" for command \"%2\".")
            .arg(shortcut.toString(),
                 command.name),
            LogWarning);
        delete s;
        return;
    }

    connect( s, SIGNAL(activated(QxtGlobalShortcut*)),
             this, SLOT(shortcutActivated(QxtGlobalShortcut*)) );

    m_shortcutActions[s] = command;
#endif
}

bool ClipboardServer::eventFilter(QObject *object, QEvent *ev)
{
    const QEvent::Type type = ev->type();

    if ( m_ignoreKeysTimer.isActive()
         && (type == QEvent::KeyPress
             || type == QEvent::Shortcut
             || type == QEvent::ShortcutOverride) )
    {
        ev->accept();
        return true;
    }

    // Close menu on Escape key and give focus back to search edit or browser.
    if (type == QEvent::KeyPress) {
        QKeyEvent *keyevent = static_cast<QKeyEvent *>(ev);
        if (keyevent->key() == Qt::Key_Escape) {
            QMenu *menu = qobject_cast<QMenu*>(object);
            if (menu != nullptr) {
                menu->close();
                if (m_wnd->browseMode())
                    m_wnd->enterBrowseMode();
                else
                    m_wnd->enterSearchMode();
            }
        }
    } else if (type == QEvent::Paint) {
        setActivePaintDevice(object);
    }

    return false;
}

void ClipboardServer::loadSettings()
{
    // reload clipboard monitor configuration
    if ( isMonitoring() )
        loadMonitorSettings();
}

void ClipboardServer::shortcutActivated(QxtGlobalShortcut *shortcut)
{
#ifdef NO_GLOBAL_SHORTCUTS
    Q_UNUSED(shortcut);
#else
    m_ignoreKeysTimer.start();

    const QMap<QxtGlobalShortcut*, Command>::const_iterator it =
            m_shortcutActions.constFind(shortcut);
    if ( it != m_shortcutActions.constEnd() ) {
        QVariantMap data;
        const QString shortcutText = portableShortcutText(shortcut->shortcut());
        data.insert(mimeShortcut, shortcutText.toUtf8());
        m_wnd->action(data, it.value());
    }
#endif
}
