#ifndef AAPSOCKET_H
#define AAPSOCKET_H

#include <QObject>
#include <QByteArray>
#include <QString>

class QSocketNotifier;

// Minimal L2CAP socket that connects straight to the AirPods AAP service PSM
// (0x1001). QBluetoothSocket can only connect by UUID, which forces an SDP
// lookup; going straight to the PSM avoids that (and its multi-second retries
// right after the Bluetooth link comes up).
class AapSocket : public QObject
{
    Q_OBJECT
public:
    static constexpr quint16 kAapPsm = 0x1001;

    explicit AapSocket(QObject *parent = nullptr);
    ~AapSocket() override;

    void connectToDevice(const QString &address, quint16 psm = kAapPsm);
    bool isOpen() const { return m_fd >= 0 && m_connected; }
    QString address() const { return m_address; }

    qint64 write(const QByteArray &data);
    QByteArray readAll();
    void close();

signals:
    void connected();
    void readyRead();
    void errorOccurred(const QString &message);

private slots:
    void onReadable();
    void onWritable();

private:
    void fail(const QString &message);
    void cleanupSocket();

    int m_fd = -1;
    bool m_connected = false;
    QString m_address;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    QByteArray m_readBuffer;
};

#endif // AAPSOCKET_H
