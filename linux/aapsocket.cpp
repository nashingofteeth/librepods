#include "aapsocket.h"
#include "logger.h"

#include <QSocketNotifier>
#include <QStringList>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

namespace {

// BlueZ stores bdaddr_t in little-endian order (b[0] is the last octet).
bool parseBdAddr(const QString &address, bdaddr_t &out)
{
    const QStringList parts = address.split(':');
    if (parts.size() != 6)
    {
        return false;
    }
    for (int i = 0; i < 6; ++i)
    {
        bool ok = false;
        const int value = parts.at(i).toInt(&ok, 16);
        if (!ok || value < 0 || value > 0xFF)
        {
            return false;
        }
        out.b[5 - i] = static_cast<quint8>(value);
    }
    return true;
}

QString errnoString(int code)
{
    return QString::fromLocal8Bit(std::strerror(code));
}

} // namespace

AapSocket::AapSocket(QObject *parent) : QObject(parent) {}

AapSocket::~AapSocket()
{
    cleanupSocket();
}

void AapSocket::connectToDevice(const QString &address, quint16 psm)
{
    cleanupSocket();

    bdaddr_t bdaddr;
    if (!parseBdAddr(address, bdaddr))
    {
        fail(QStringLiteral("invalid Bluetooth address: ") + address);
        return;
    }
    m_address = address;

    m_fd = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (m_fd < 0)
    {
        fail(QStringLiteral("socket(): ") + errnoString(errno));
        return;
    }

    const int flags = ::fcntl(m_fd, F_GETFL, 0);
    ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
    ::fcntl(m_fd, F_SETFD, FD_CLOEXEC);

    sockaddr_l2 addr{};
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = htobs(psm);
    addr.l2_bdaddr = bdaddr;
    addr.l2_cid = 0;
    addr.l2_bdaddr_type = BDADDR_BREDR;

    const int rc = ::connect(m_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (rc == 0)
    {
        m_connected = true;
        m_readNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_readNotifier, &QSocketNotifier::activated, this, &AapSocket::onReadable);
        emit connected();
        return;
    }

    if (errno != EINPROGRESS && errno != EAGAIN)
    {
        fail(QStringLiteral("connect(): ") + errnoString(errno));
        return;
    }

    // Non-blocking connect in progress; wait until the socket is writable.
    m_writeNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Write, this);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &AapSocket::onWritable);
}

void AapSocket::onWritable()
{
    int soError = 0;
    socklen_t len = sizeof(soError);
    if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &soError, &len) < 0)
    {
        fail(QStringLiteral("getsockopt(): ") + errnoString(errno));
        return;
    }
    if (soError != 0)
    {
        fail(QStringLiteral("connect(): ") + errnoString(soError));
        return;
    }

    if (m_writeNotifier)
    {
        m_writeNotifier->setEnabled(false);
        m_writeNotifier->deleteLater();
        m_writeNotifier = nullptr;
    }

    m_connected = true;
    m_readNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &AapSocket::onReadable);
    emit connected();
}

void AapSocket::onReadable()
{
    char buffer[4096];
    while (true)
    {
        const ssize_t n = ::recv(m_fd, buffer, sizeof(buffer), 0);
        if (n > 0)
        {
            m_readBuffer.append(buffer, static_cast<int>(n));
            continue;
        }
        if (n == 0)
        {
            // Remote closed the channel.
            close();
            emit errorOccurred(QStringLiteral("remote host closed the connection"));
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }
        if (errno == EINTR)
        {
            continue;
        }
        fail(QStringLiteral("recv(): ") + errnoString(errno));
        return;
    }

    if (!m_readBuffer.isEmpty())
    {
        emit readyRead();
    }
}

QByteArray AapSocket::readAll()
{
    const QByteArray data = m_readBuffer;
    m_readBuffer.clear();
    return data;
}

qint64 AapSocket::write(const QByteArray &data)
{
    if (!isOpen())
    {
        return -1;
    }

    const char *ptr = data.constData();
    qint64 remaining = data.size();
    while (remaining > 0)
    {
        const ssize_t n = ::send(m_fd, ptr, static_cast<size_t>(remaining), MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // Send buffer full; AAP packets are small so this is very rare.
                LOG_WARN("AapSocket send buffer full, dropping " << remaining << " bytes");
                break;
            }
            fail(QStringLiteral("send(): ") + errnoString(errno));
            return -1;
        }
        ptr += n;
        remaining -= n;
    }
    return data.size();
}

void AapSocket::close()
{
    cleanupSocket();
}

void AapSocket::fail(const QString &message)
{
    cleanupSocket();
    LOG_ERROR("AapSocket error: " << message);
    emit errorOccurred(message);
}

void AapSocket::cleanupSocket()
{
    if (m_readNotifier)
    {
        m_readNotifier->setEnabled(false);
        m_readNotifier->deleteLater();
        m_readNotifier = nullptr;
    }
    if (m_writeNotifier)
    {
        m_writeNotifier->setEnabled(false);
        m_writeNotifier->deleteLater();
        m_writeNotifier = nullptr;
    }
    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
    m_connected = false;
    m_readBuffer.clear();
}
