#ifndef BLUETOOTHMONITOR_H
#define BLUETOOTHMONITOR_H

#include <QObject>
#include <QHash>
#include <QtDBus/QtDBus>

// Forward declarations for D-Bus types
typedef QMap<QDBusObjectPath, QMap<QString, QVariantMap>> ManagedObjectList;
Q_DECLARE_METATYPE(ManagedObjectList)

class BluetoothMonitor : public QObject, protected QDBusContext
{
    Q_OBJECT
public:
    explicit BluetoothMonitor(QObject *parent = nullptr);
    ~BluetoothMonitor();

    bool checkAlreadyConnectedDevices();

signals:
    void deviceConnected(const QString &macAddress, const QString &deviceName);
    void deviceDisconnected(const QString &macAddress, const QString &deviceName);

private slots:
    void onPropertiesChanged(const QString &interface, const QVariantMap &changedProps, const QStringList &invalidatedProps);
    void onInterfacesAdded(const QDBusObjectPath &objectPath, const QMap<QString, QVariantMap> &interfaces);

private:
    QDBusConnection m_dbus;
    void registerDBusService();
    bool isAirPodsDevice(const QString &devicePath);
    QString getDeviceName(const QString &devicePath);
    QString getDeviceAddress(const QString &devicePath);

    // Property caches, keyed by BlueZ object path. Populated from
    // GetManagedObjects / InterfacesAdded so the connect path
    // (PropertiesChanged -> Connected) does not block on D-Bus Get calls.
    QHash<QString, bool> m_isAirPodsCache;
    QHash<QString, QString> m_deviceNames;
    QHash<QString, QString> m_deviceAddresses;
};

#endif // BLUETOOTHMONITOR_H
