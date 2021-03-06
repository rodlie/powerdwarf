/*
# PowerKit <https://github.com/rodlie/powerkit>
# Copyright (c) 2018, Ole-André Rodlie <ole.andre.rodlie@gmail.com> All rights reserved.
#
# Available under the 3-clause BSD license
# See the LICENSE file for full details
*/

#include "powerkit.h"
#include "def.h"

#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingReply>
#include <QXmlStreamReader>
#include <QProcess>
#include <QMapIterator>
#include <QDebug>
#include <QDBusReply>

PowerKit::PowerKit(QObject *parent) : QObject(parent)
  , upower(0)
  , logind(0)
  , ckit(0)
  , pmd(0)
  , wasDocked(false)
  , wasLidClosed(false)
  , wasOnBattery(false)
  , wakeAlarm(false)
  , suspendWakeupBattery(0)
  , suspendWakeupAC(0)
  , lockScreenOnSuspend(true)
  , lockScreenOnResume(false)
{
    setup();
    timer.setInterval(TIMEOUT_CHECK);
    connect(&timer, SIGNAL(timeout()),
            this, SLOT(check()));
    timer.start();
}

PowerKit::~PowerKit()
{
    clearDevices();
    releaseSuspendLock();
}

QMap<QString, Device *> PowerKit::getDevices()
{
    return devices;
}

bool PowerKit::availableService(const QString &service,
                          const QString &path,
                          const QString &interface)
{
    QDBusInterface iface(service,
                         path,
                         interface,
                         QDBusConnection::systemBus());
    if (iface.isValid()) { return true; }
    return false;
}

bool PowerKit::availableAction(const PowerKit::PKMethod &method,
                               const PowerKit::PKBackend &backend)
{
    QString service, path, interface, cmd;
    switch (backend) {
    case PKConsoleKit:
        service = CONSOLEKIT_SERVICE;
        path = CONSOLEKIT_PATH;
        interface = CONSOLEKIT_MANAGER;
        break;
    case PKLogind:
        service = LOGIND_SERVICE;
        path = LOGIND_PATH;
        interface = LOGIND_MANAGER;
        break;
    case PKUPower:
        service = UPOWER_SERVICE;
        path = UPOWER_PATH;
        interface = UPOWER_MANAGER;
        break;
    default:
        return false;
    }
    switch (method) {
    case PKCanRestart:
        cmd = PK_CAN_RESTART;
        break;
    case PKCanPowerOff:
        cmd = PK_CAN_POWEROFF;
        break;
    case PKCanSuspend:
        cmd = PK_CAN_SUSPEND;
        break;
    case PKCanHibernate:
        cmd = PK_CAN_HIBERNATE;
        break;
    case PKCanHybridSleep:
        cmd = PK_CAN_HYBRIDSLEEP;
        break;
    case PKSuspendAllowed:
        cmd = PK_SUSPEND_ALLOWED;
        break;
    case PKHibernateAllowed:
        cmd = PK_HIBERNATE_ALLOWED;
        break;
    default:
        return false;
    }
    QDBusInterface iface(service, path, interface,
                         QDBusConnection::systemBus());
    if (!iface.isValid()) { return false; }
    QDBusMessage reply = iface.call(cmd);
    if (reply.arguments().first().toString() == DBUS_OK_REPLY) { return true; }
    bool result = reply.arguments().first().toBool();
    if (!reply.errorMessage().isEmpty()) { result = false; }
    return result;
}

QString PowerKit::executeAction(const PowerKit::PKAction &action,
                                const PowerKit::PKBackend &backend)
{
    QString service, path, interface, cmd;
    switch (backend) {
    case PKConsoleKit:
        service = CONSOLEKIT_SERVICE;
        path = CONSOLEKIT_PATH;
        interface = CONSOLEKIT_MANAGER;
        break;
    case PKLogind:
        service = LOGIND_SERVICE;
        path = LOGIND_PATH;
        interface = LOGIND_MANAGER;
        break;
    case PKUPower:
        service = UPOWER_SERVICE;
        path = UPOWER_PATH;
        interface = UPOWER_MANAGER;
        break;
    default:
        return QObject::tr(PK_NO_BACKEND);
    }
    switch (action) {
    case PKRestartAction:
        cmd = PK_RESTART;
        break;
    case PKPowerOffAction:
        cmd = PK_POWEROFF;
        break;
    case PKSuspendAction:
        cmd = PK_SUSPEND;
        break;
    case PKHibernateAction:
        cmd = PK_HIBERNATE;
        break;
    case PKHybridSleepAction:
        cmd = PK_HYBRIDSLEEP;
        break;
    default:
        return QObject::tr(PK_NO_ACTION);
    }
    QDBusInterface iface(service, path, interface,
                         QDBusConnection::systemBus());
    if (!iface.isValid()) { return QObject::tr(DBUS_FAILED_CONN); }

    QDBusMessage reply;
    if (backend == PKUPower) { reply = iface.call(cmd); }
    else { reply = iface.call(cmd, true); }

    return reply.errorMessage();
}

QStringList PowerKit::find()
{
    QStringList result;
    QDBusMessage call = QDBusMessage::createMethodCall(UPOWER_SERVICE,
                                                       QString("%1/devices").arg(UPOWER_PATH),
                                                       DBUS_INTROSPECTABLE,
                                                       "Introspect");
    QDBusPendingReply<QString> reply = QDBusConnection::systemBus().call(call);
    if (reply.isError()) {
        qWarning() << "powerkit find devices failed, check the upower service!!!";
        return result;
    }
    QList<QDBusObjectPath> objects;
    QXmlStreamReader xml(reply.value());
    if (xml.hasError()) { return result; }
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.tokenType() == QXmlStreamReader::StartElement &&
            xml.name().toString() == "node" ) {
            QString name = xml.attributes().value("name").toString();
            if (!name.isEmpty()) { objects << QDBusObjectPath(UPOWER_DEVICES + name); }
        }
    }
    foreach (QDBusObjectPath device, objects) {
        result << device.path();
    }
    return result;
}

void PowerKit::setup()
{
    QDBusConnection system = QDBusConnection::systemBus();
    if (system.isConnected()) {
        system.connect(UPOWER_SERVICE,
                       UPOWER_PATH,
                       UPOWER_SERVICE,
                       DBUS_DEVICE_ADDED,
                       this,
                       SLOT(deviceAdded(const QDBusObjectPath&)));
        system.connect(UPOWER_SERVICE,
                       UPOWER_PATH,
                       UPOWER_SERVICE,
                       DBUS_DEVICE_ADDED,
                       this,
                       SLOT(deviceAdded(const QString&)));
        system.connect(UPOWER_SERVICE,
                       UPOWER_PATH,
                       UPOWER_SERVICE,
                       DBUS_DEVICE_REMOVED,
                       this,
                       SLOT(deviceRemoved(const QDBusObjectPath&)));
        system.connect(UPOWER_SERVICE,
                       UPOWER_PATH,
                       UPOWER_SERVICE,
                       DBUS_DEVICE_REMOVED,
                       this,
                       SLOT(deviceRemoved(const QString&)));
        system.connect(UPOWER_SERVICE,
                       UPOWER_PATH,
                       UPOWER_SERVICE,
                       DBUS_CHANGED,
                       this,
                       SLOT(deviceChanged()));
        system.connect(UPOWER_SERVICE,
                       UPOWER_PATH,
                       UPOWER_SERVICE,
                       DBUS_DEVICE_CHANGED,
                       this,
                       SLOT(deviceChanged()));
        system.connect(UPOWER_SERVICE,
                       UPOWER_PATH,
                       UPOWER_SERVICE,
                       UPOWER_NOTIFY_RESUME,
                       this,
                       SLOT(handleResume()));
        system.connect(UPOWER_SERVICE,
                       UPOWER_PATH,
                       UPOWER_SERVICE,
                       UPOWER_NOTIFY_SLEEP,
                       this,
                       SLOT(handleSuspend()));
        system.connect(LOGIND_SERVICE,
                       LOGIND_PATH,
                       LOGIND_MANAGER,
                       PK_PREPARE_FOR_SUSPEND,
                       this,
                       SLOT(handlePrepareForSuspend(bool)));
        system.connect(CONSOLEKIT_SERVICE,
                       CONSOLEKIT_PATH,
                       CONSOLEKIT_MANAGER,
                       PK_PREPARE_FOR_SLEEP,
                       this,
                       SLOT(handlePrepareForSuspend(bool)));
        if (upower == NULL) {
            upower = new QDBusInterface(UPOWER_SERVICE,
                                        UPOWER_PATH,
                                        UPOWER_MANAGER,
                                        system,
                                        this);
        }
        if (logind == NULL) {
            logind = new QDBusInterface(LOGIND_SERVICE,
                                        LOGIND_PATH,
                                        LOGIND_MANAGER,
                                        system,
                                        this);
        }
        if (ckit == NULL) {
            ckit = new QDBusInterface(CONSOLEKIT_SERVICE,
                                      CONSOLEKIT_PATH,
                                      CONSOLEKIT_MANAGER,
                                      system,
                                      this);
        }
        if (pmd == NULL) {
            pmd = new QDBusInterface(PMD_SERVICE,
                                     PMD_PATH,
                                     PMD_MANAGER,
                                     system,
                                     this);
        }
        if (!suspendLock) { registerSuspendLock(); }
        scan();
    }
}

void PowerKit::check()
{
    if (!QDBusConnection::systemBus().isConnected()) {
        setup();
        return;
    }
    if (!suspendLock) { registerSuspendLock(); }
    if (!upower->isValid()) { scan(); }
}

void PowerKit::scan()
{
    QStringList foundDevices = find();
    for (int i=0; i < foundDevices.size(); i++) {
        QString foundDevicePath = foundDevices.at(i);
        if (devices.contains(foundDevicePath)) { continue; }
        Device *newDevice = new Device(foundDevicePath, this);
        connect(newDevice,
                SIGNAL(deviceChanged(QString)),
                this,
                SLOT(handleDeviceChanged(QString)));
        devices[foundDevicePath] = newDevice;
    }
    UpdateDevices();
    emit UpdatedDevices();
}

void PowerKit::deviceAdded(const QDBusObjectPath &obj)
{
    deviceAdded(obj.path());
}

void PowerKit::deviceAdded(const QString &path)
{
    if (!upower->isValid()) { return; }
    if (path.startsWith(QString(DBUS_JOBS).arg(UPOWER_PATH))) { return; }
    emit DeviceWasAdded(path);
    scan();
}

void PowerKit::deviceRemoved(const QDBusObjectPath &obj)
{
    deviceRemoved(obj.path());
}

void PowerKit::deviceRemoved(const QString &path)
{
    if (!upower->isValid()) { return; }
    bool deviceExists = devices.contains(path);
    if (path.startsWith(QString(DBUS_JOBS).arg(UPOWER_PATH))) { return; }
    if (deviceExists) {
        if (find().contains(path)) { return; }
        delete devices.take(path);
        emit DeviceWasRemoved(path);
    }
    scan();
}

void PowerKit::deviceChanged()
{
    if (wasLidClosed != LidIsClosed()) {
        if (!wasLidClosed && LidIsClosed()) {
            emit LidClosed();
        } else if (wasLidClosed && !LidIsClosed()) {
            emit LidOpened();
        }
    }
    wasLidClosed = LidIsClosed();

    if (wasOnBattery != OnBattery()) {
        if (!wasOnBattery && OnBattery()) {
            emit SwitchedToBattery();
        } else if (wasOnBattery && !OnBattery()) {
            emit SwitchedToAC();
        }
    }
    wasOnBattery = OnBattery();

    emit UpdatedDevices();
}

void PowerKit::handleDeviceChanged(const QString &device)
{
    if (device.isEmpty()) { return; }
    deviceChanged();
}

void PowerKit::handleResume()
{
    if (HasLogind() || HasConsoleKit()) { return; }
    qDebug() << "handle resume from upower";
    handlePrepareForSuspend(false);
}

void PowerKit::handleSuspend()
{
    if (HasLogind() || HasConsoleKit()) { return; }
    qDebug() << "handle suspend from upower";
    if (lockScreenOnSuspend) { LockScreen(); }
    emit PrepareForSuspend();
}

void PowerKit::handlePrepareForSuspend(bool prepare)
{
    qDebug() << "handle prepare for suspend/resume from consolekit/logind" << prepare;
    if (prepare) {
        if (lockScreenOnSuspend) { LockScreen(); }
        emit PrepareForSuspend();
        releaseSuspendLock(); // we are ready for suspend
    }
    else { // resume
        UpdateDevices();
        if (lockScreenOnResume) { LockScreen(); }
        if (hasWakeAlarm() &&
             wakeAlarmDate.isValid() &&
             CanHibernate())
        {
            qDebug() << "we may have a wake alarm" << wakeAlarmDate;
            QDateTime currentDate = QDateTime::currentDateTime();
            if (currentDate>=wakeAlarmDate && wakeAlarmDate.secsTo(currentDate)<300) {
                qDebug() << "wake alarm is active, that means we should hibernate";
                clearWakeAlarm();
                Hibernate();
                return;
            }
        }
        clearWakeAlarm();
        emit PrepareForResume();
    }
}

void PowerKit::clearDevices()
{
    QMapIterator<QString, Device*> device(devices);
    while (device.hasNext()) {
        device.next();
        delete device.value();
    }
    devices.clear();
}

void PowerKit::handleNewInhibitScreenSaver(const QString &application, const QString &reason, quint32 cookie)
{
    Q_UNUSED(reason)
    ssInhibitors[cookie] = application;
    emit UpdatedInhibitors();
}

void PowerKit::handleNewInhibitPowerManagement(const QString &application, const QString &reason, quint32 cookie)
{
    Q_UNUSED(reason)
    pmInhibitors[cookie] = application;
    emit UpdatedInhibitors();
}

void PowerKit::handleDelInhibitScreenSaver(quint32 cookie)
{
    if (ssInhibitors.contains(cookie)) {
        ssInhibitors.remove(cookie);
        emit UpdatedInhibitors();
    }
}

void PowerKit::handleDelInhibitPowerManagement(quint32 cookie)
{
    if (pmInhibitors.contains(cookie)) {
        pmInhibitors.remove(cookie);
        emit UpdatedInhibitors();
    }
}

bool PowerKit::registerSuspendLock()
{
    if (suspendLock) { return false; }
    qDebug() << "register suspend lock";
    QDBusReply<QDBusUnixFileDescriptor> reply;
    if (HasLogind() && logind->isValid()) {
        reply = ckit->call("Inhibit",
                           "sleep",
                           "powerkit",
                           "Lock screen etc",
                           "delay");
    } else if (HasConsoleKit() && ckit->isValid()) {
        reply = ckit->call("Inhibit",
                           "sleep",
                           "powerkit",
                           "Lock screen etc",
                           "delay");
    }
    if (reply.isValid()) {
        suspendLock.reset(new QDBusUnixFileDescriptor(reply.value()));
        return true;
    } else {
        qDebug() << reply.error();
    }
    return false;
}

void PowerKit::setWakeAlarmFromSettings()
{
    if (!CanHibernate()) { return; }
    int wmin = OnBattery()?suspendWakeupBattery:suspendWakeupAC;
    if (wmin>0) {
        qDebug() << "we need to set a wake alarm" << wmin << "min from now";
        QDateTime date = QDateTime::currentDateTime().addSecs(wmin*60);
        setWakeAlarm(date);
    }
}

bool PowerKit::HasConsoleKit()
{
    return availableService(CONSOLEKIT_SERVICE,
                            CONSOLEKIT_PATH,
                            CONSOLEKIT_MANAGER);
}

bool PowerKit::HasLogind()
{
    return availableService(LOGIND_SERVICE,
                            LOGIND_PATH,
                            LOGIND_MANAGER);
}

bool PowerKit::HasUPower()
{
    return availableService(UPOWER_SERVICE,
                            UPOWER_PATH,
                            UPOWER_MANAGER);
}

bool PowerKit::hasPMD()
{
    return availableService(PMD_SERVICE,
                            PMD_PATH,
                            PMD_MANAGER);
}

bool PowerKit::hasWakeAlarm()
{
    return wakeAlarm;
}

bool PowerKit::CanRestart()
{
    if (HasLogind()) {
        return availableAction(PKCanRestart, PKLogind);
    } else if (HasConsoleKit()) {
        return availableAction(PKCanRestart, PKConsoleKit);
    }
    return false;
}

bool PowerKit::CanPowerOff()
{
    if (HasLogind()) {
        return availableAction(PKCanPowerOff, PKLogind);
    } else if (HasConsoleKit()) {
        return availableAction(PKCanPowerOff, PKConsoleKit);
    }
    return false;
}

bool PowerKit::CanSuspend()
{
    if (HasLogind()) {
        return availableAction(PKCanSuspend, PKLogind);
    } else if (HasConsoleKit()) {
        return availableAction(PKCanSuspend, PKConsoleKit);
    } else if (HasUPower()) {
        return availableAction(PKSuspendAllowed, PKUPower);
    }
    return false;
}

bool PowerKit::CanHibernate()
{
    if (HasLogind()) {
        return availableAction(PKCanHibernate, PKLogind);
    } else if (HasConsoleKit()) {
        return availableAction(PKCanHibernate, PKConsoleKit);
    } else if (HasUPower()) {
        return availableAction(PKHibernateAllowed, PKUPower);
    }
    return false;
}

bool PowerKit::CanHybridSleep()
{
    if (HasLogind()) {
        return availableAction(PKCanHybridSleep, PKLogind);
    } else if (HasConsoleKit()) {
        return availableAction(PKCanHybridSleep, PKConsoleKit);
    }
    return false;
}

QString PowerKit::Restart()
{
    qDebug() << "try to restart";
    if (HasLogind()) {
        return executeAction(PKRestartAction, PKLogind);
    } else if (HasConsoleKit()) {
        return executeAction(PKRestartAction, PKConsoleKit);
    }
    return QObject::tr(PK_NO_BACKEND);
}

QString PowerKit::PowerOff()
{
    qDebug() << "try to poweroff";
    if (HasLogind()) {
        return executeAction(PKPowerOffAction, PKLogind);
    } else if (HasConsoleKit()) {
        return executeAction(PKPowerOffAction, PKConsoleKit);
    }
    return QObject::tr(PK_NO_BACKEND);
}

QString PowerKit::Suspend()
{
    qDebug() << "try to suspend";
    if (lockScreenOnSuspend) { LockScreen(); }
    if (HasLogind()) {
        setWakeAlarmFromSettings();
        return executeAction(PKSuspendAction, PKLogind);
    } else if (HasConsoleKit()) {
        setWakeAlarmFromSettings();
        return executeAction(PKSuspendAction, PKConsoleKit);
    } else if (HasUPower()) {
        return executeAction(PKSuspendAction, PKUPower);
    }
    return QObject::tr(PK_NO_BACKEND);
}

QString PowerKit::Hibernate()
{
    qDebug() << "try to hibernate";
    if (lockScreenOnSuspend) { LockScreen(); }
    if (HasLogind()) {
        return executeAction(PKHibernateAction, PKLogind);
    } else if (HasConsoleKit()) {
        return executeAction(PKHibernateAction, PKConsoleKit);
    } else if (HasUPower()) {
        return executeAction(PKHibernateAction, PKUPower);
    }
    return QObject::tr(PK_NO_BACKEND);
}

QString PowerKit::HybridSleep()
{
    qDebug() << "try to hybridsleep";
    if (lockScreenOnSuspend) { LockScreen(); }
    if (HasLogind()) {
        return executeAction(PKHybridSleepAction, PKLogind);
    } else if (HasConsoleKit()) {
        return executeAction(PKHybridSleepAction, PKConsoleKit);
    }
    return QObject::tr(PK_NO_BACKEND);
}

bool PowerKit::setWakeAlarm(const QDateTime &date)
{
    if (pmd && date.isValid() && CanHibernate()) {
        if (!pmd->isValid()) { return false; }
        QDBusMessage reply = pmd->call("setWakeAlarm",
                                       date.toString("yyyy-MM-dd HH:mm:ss"));
        bool alarm = reply.arguments().first().toBool() && reply.errorMessage().isEmpty();
        qDebug() << "WAKE OK?" << alarm;
        wakeAlarm = alarm;
        if (alarm) {
            qDebug() << "wake alarm was set to" << date;
            wakeAlarmDate = date;
        }
        return alarm;
    }
    return false;
}

void PowerKit::clearWakeAlarm()
{
    wakeAlarm = false;
}

bool PowerKit::IsDocked()
{
    if (logind->isValid()) { return logind->property(LOGIND_DOCKED).toBool(); }
    if (upower->isValid()) { return upower->property(UPOWER_DOCKED).toBool(); }
    return false;
}

bool PowerKit::LidIsPresent()
{
    if (upower->isValid()) { return upower->property(UPOWER_LID_IS_PRESENT).toBool(); }
    return false;
}

bool PowerKit::LidIsClosed()
{
    if (upower->isValid()) { return upower->property(UPOWER_LID_IS_CLOSED).toBool(); }
    return false;
}

bool PowerKit::OnBattery()
{
    if (upower->isValid()) { return upower->property(UPOWER_ON_BATTERY).toBool(); }
    return false;
}

double PowerKit::BatteryLeft()
{
    if (OnBattery()) { UpdateBattery(); }
    double batteryLeft = 0;
    QMapIterator<QString, Device*> device(devices);
    int batteries = 0;
    while (device.hasNext()) {
        device.next();
        if (device.value()->isBattery &&
            device.value()->isPresent &&
            !device.value()->nativePath.isEmpty())
        {
            batteryLeft += device.value()->percentage;
            batteries++;
        } else { continue; }
    }
    return batteryLeft/batteries;
}

void PowerKit::LockScreen()
{
    qDebug() << "lock screen";
    QProcess proc;
    proc.start(XSCREENSAVER_LOCK);
    proc.waitForFinished();
    proc.close();
}

bool PowerKit::HasBattery()
{
    QMapIterator<QString, Device*> device(devices);
    while (device.hasNext()) {
        device.next();
        if (device.value()->isBattery) { return true; }
    }
    return false;
}

qlonglong PowerKit::TimeToEmpty()
{
    if (OnBattery()) { UpdateBattery(); }
    qlonglong result = 0;
    QMapIterator<QString, Device*> device(devices);
    while (device.hasNext()) {
        device.next();
        if (device.value()->isBattery &&
            device.value()->isPresent &&
            !device.value()->nativePath.isEmpty())
        { result += device.value()->timeToEmpty; }
    }
    return result;
}

qlonglong PowerKit::TimeToFull()
{
    if (OnBattery()) { UpdateBattery(); }
    qlonglong result = 0;
    QMapIterator<QString, Device*> device(devices);
    while (device.hasNext()) {
        device.next();
        if (device.value()->isBattery &&
            device.value()->isPresent &&
            !device.value()->nativePath.isEmpty())
        { result += device.value()->timeToFull; }
    }
    return result;
}

void PowerKit::UpdateDevices()
{
    QMapIterator<QString, Device*> device(devices);
    while (device.hasNext()) {
        device.next();
        device.value()->update();
    }
}

void PowerKit::UpdateBattery()
{
    QMapIterator<QString, Device*> device(devices);
    while (device.hasNext()) {
        device.next();
        if (device.value()->isBattery) {
            device.value()->updateBattery();
        }
    }
}

void PowerKit::UpdateConfig()
{
    emit Update();
}

QStringList PowerKit::ScreenSaverInhibitors()
{
    QStringList result;
    QMapIterator<quint32, QString> i(ssInhibitors);
    while (i.hasNext()) {
        i.next();
        result << i.value();
    }
    return result;
}

QStringList PowerKit::PowerManagementInhibitors()
{
    QStringList result;
    QMapIterator<quint32, QString> i(pmInhibitors);
    while (i.hasNext()) {
        i.next();
        result << i.value();
    }
    return result;
}

const QDateTime PowerKit::getWakeAlarm()
{
    return wakeAlarmDate;
}

void PowerKit::releaseSuspendLock()
{
    qDebug() << "release suspend lock";
    suspendLock.reset(NULL);
}

void PowerKit::setSuspendWakeAlarmOnBattery(int value)
{
    qDebug() << "set suspend wake alarm on battery" << value;
    suspendWakeupBattery = value;
}

void PowerKit::setSuspendWakeAlarmOnAC(int value)
{
    qDebug() << "set suspend wake alarm on ac" << value;
    suspendWakeupAC = value;
}

void PowerKit::setLockScreenOnSuspend(bool lock)
{
    qDebug() << "set lock screen on suspend" << lock;
    lockScreenOnSuspend = lock;
}

void PowerKit::setLockScreenOnResume(bool lock)
{
    qDebug() << "set lock screen on resume" << lock;
    lockScreenOnResume = lock;
}
