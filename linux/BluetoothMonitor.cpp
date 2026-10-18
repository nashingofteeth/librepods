#include "BluetoothMonitor.h"
#include "logger.h"

#include <QDebug>
#include <QDBusObjectPath>
#include <QDBusMetaType>

namespace {
// Apple AAP L2CAP service UUID advertised by AirPods.
const QString kAirPodsServiceUuid = QStringLiteral("74ec2172-0bad-4d01-8f77-997b2be0722a");
}

BluetoothMonitor::BluetoothMonitor(QObject *parent)
    : QObject(parent), m_dbus(QDBusConnection::systemBus())
{
    // Register meta-types for D-Bus interaction
    qDBusRegisterMetaType<QDBusObjectPath>();
    qDBusRegisterMetaType<ManagedObjectList>();

    if (!m_dbus.isConnected())
    {
        LOG_WARN("Failed to connect to system D-Bus");
        return;
    }

    registerDBusService();
    checkAlreadyConnectedDevices(); // Check for already connected devices on startup
}

BluetoothMonitor::~BluetoothMonitor()
{
    m_dbus.disconnectFromBus(m_dbus.name());
}

void BluetoothMonitor::registerDBusService()
{
    // Match signals for PropertiesChanged on any BlueZ Device interface
    if (!m_dbus.connect("", "", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                        this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList))))
    {
        LOG_WARN("Failed to connect to D-Bus PropertiesChanged signal");
    }

    // Warm the device property cache as devices appear, so that the connect
    // path never has to issue blocking D-Bus Get calls.
    if (!m_dbus.connect("org.bluez", "", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                        this, SLOT(onInterfacesAdded(QDBusObjectPath, QMap<QString, QVariantMap>))))
    {
        LOG_WARN("Failed to connect to D-Bus InterfacesAdded signal");
    }
}

bool BluetoothMonitor::isAirPodsDevice(const QString &devicePath)
{
    const auto cached = m_isAirPodsCache.constFind(devicePath);
    if (cached != m_isAirPodsCache.constEnd())
    {
        return cached.value();
    }

    QDBusInterface deviceInterface("org.bluez", devicePath, "org.freedesktop.DBus.Properties", m_dbus);

    // Get UUIDs to check if it's an AirPods device
    QDBusReply<QVariant> uuidsReply = deviceInterface.call("Get", "org.bluez.Device1", "UUIDs");
    if (!uuidsReply.isValid())
    {
        // Don't cache a transient failure; the device may not be ready yet.
        return false;
    }

    const bool isAirPods = uuidsReply.value().toStringList().contains(kAirPodsServiceUuid);
    m_isAirPodsCache.insert(devicePath, isAirPods);
    return isAirPods;
}

QString BluetoothMonitor::getDeviceName(const QString &devicePath)
{
    const auto cached = m_deviceNames.constFind(devicePath);
    if (cached != m_deviceNames.constEnd())
    {
        return cached.value();
    }

    QDBusInterface deviceInterface("org.bluez", devicePath, "org.freedesktop.DBus.Properties", m_dbus);
    QDBusReply<QVariant> nameReply = deviceInterface.call("Get", "org.bluez.Device1", "Name");
    if (nameReply.isValid())
    {
        const QString name = nameReply.value().toString();
        m_deviceNames.insert(devicePath, name);
        return name;
    }
    return "Unknown";
}

QString BluetoothMonitor::getDeviceAddress(const QString &devicePath)
{
    const auto cached = m_deviceAddresses.constFind(devicePath);
    if (cached != m_deviceAddresses.constEnd())
    {
        return cached.value();
    }

    QDBusInterface deviceInterface("org.bluez", devicePath, "org.freedesktop.DBus.Properties", m_dbus);
    QDBusReply<QVariant> addrReply = deviceInterface.call("Get", "org.bluez.Device1", "Address");
    if (!addrReply.isValid())
    {
        return QString();
    }

    const QString address = addrReply.value().toString();
    m_deviceAddresses.insert(devicePath, address);
    return address;
}

bool BluetoothMonitor::checkAlreadyConnectedDevices()
{
    QDBusInterface objectManager("org.bluez", "/", "org.freedesktop.DBus.ObjectManager", m_dbus);
    QDBusMessage reply = objectManager.call("GetManagedObjects");

    if (reply.type() == QDBusMessage::ErrorMessage)
    {
        LOG_WARN("Failed to get managed objects: " << reply.errorMessage());
        return false;
    }

    QVariant firstArg = reply.arguments().constFirst();
    QDBusArgument arg = firstArg.value<QDBusArgument>();
    ManagedObjectList managedObjects;
    arg >> managedObjects;

    bool deviceFound = false;

    for (auto it = managedObjects.constBegin(); it != managedObjects.constEnd(); ++it)
    {
        const QDBusObjectPath &objPath = it.key();
        const QMap<QString, QVariantMap> &interfaces = it.value();

        if (interfaces.contains("org.bluez.Device1"))
        {
            const QVariantMap &deviceProps = interfaces.value("org.bluez.Device1");

            // Check if the device has the necessary properties
            if (!deviceProps.contains("UUIDs") || !deviceProps.contains("Connected") ||
                !deviceProps.contains("Address") || !deviceProps.contains("Name"))
            {
                continue;
            }

            QStringList uuids = deviceProps["UUIDs"].toStringList();
            bool isAirPods = uuids.contains(kAirPodsServiceUuid);

            // Cache everything we just read so the connect path can avoid Gets.
            m_isAirPodsCache.insert(objPath.path(), isAirPods);
            m_deviceNames.insert(objPath.path(), deviceProps["Name"].toString());
            m_deviceAddresses.insert(objPath.path(), deviceProps["Address"].toString());

            if (isAirPods)
            {
                bool connected = deviceProps["Connected"].toBool();
                if (connected)
                {
                    QString macAddress = deviceProps["Address"].toString();
                    QString deviceName = deviceProps["Name"].toString();
                    emit deviceConnected(macAddress, deviceName);
                    LOG_DEBUG("Found already connected AirPods: " << macAddress << " Name: " << deviceName);
                    deviceFound = true;
                }
            }
        }
    }
    return deviceFound;
}

void BluetoothMonitor::onInterfacesAdded(const QDBusObjectPath &objectPath, const QMap<QString, QVariantMap> &interfaces)
{
    const QString path = objectPath.path();
    if (path.isEmpty() || !interfaces.contains("org.bluez.Device1"))
    {
        return;
    }

    const QVariantMap &props = interfaces.value("org.bluez.Device1");

    // Only cache the AirPods flag once UUIDs are known, so we never poison the
    // cache with a "false" for a device whose UUIDs have not been populated yet.
    if (props.contains("UUIDs"))
    {
        m_isAirPodsCache.insert(path, props["UUIDs"].toStringList().contains(kAirPodsServiceUuid));
    }
    if (props.contains("Name"))
    {
        m_deviceNames.insert(path, props["Name"].toString());
    }
    if (props.contains("Address"))
    {
        m_deviceAddresses.insert(path, props["Address"].toString());
    }
}

void BluetoothMonitor::onPropertiesChanged(const QString &interface, const QVariantMap &changedProps, const QStringList &invalidatedProps)
{
    Q_UNUSED(invalidatedProps);

    if (interface != "org.bluez.Device1")
    {
        return;
    }

    if (changedProps.contains("Connected"))
    {
        bool connected = changedProps["Connected"].toBool();
        QString path = QDBusContext::message().path();

        if (!isAirPodsDevice(path))
        {
            return;
        }

        // Address/Name are read from the cache populated on discovery; this
        // avoids blocking D-Bus round-trips on the connect critical path.
        QString macAddress = getDeviceAddress(path);
        if (macAddress.isEmpty())
        {
            return;
        }
        QString deviceName = getDeviceName(path);

        if (connected)
        {
            emit deviceConnected(macAddress, deviceName);
            LOG_DEBUG("AirPods device connected:" << macAddress << " Name:" << deviceName);
        }
        else
        {
            emit deviceDisconnected(macAddress, deviceName);
            LOG_DEBUG("AirPods device disconnected:" << macAddress << " Name:" << deviceName);
        }
    }
}
