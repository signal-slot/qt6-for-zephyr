#include "qzephyr_tls_socket.h"
#include <stdio.h>

#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

QZephyrTlsSocket::QZephyrTlsSocket(QObject *parent)
    : QTcpSocket(parent)
{
}

void QZephyrTlsSocket::setSecTags(const QList<int> &tags)
{
    m_secTags = tags;
}

void QZephyrTlsSocket::setPeerVerify(int mode)
{
    m_peerVerify = mode;
}

void QZephyrTlsSocket::setTlsHostname(const QString &hostname)
{
    m_tlsHostname = hostname;
}

void QZephyrTlsSocket::connectToHost(const QString &hostName, quint16 port,
                                      OpenMode openMode,
                                      NetworkLayerProtocol protocol)
{
    Q_UNUSED(protocol);

    if (m_tlsHostname.isEmpty())
        m_tlsHostname = hostName;

    int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
    if (fd < 0) {
        printf("[tls] zsock_socket(TLS_1_2) failed: %d\n", fd);
        setSocketError(QAbstractSocket::SocketAccessError);
        emit errorOccurred(QAbstractSocket::SocketAccessError);
        return;
    }

    if (!configureTlsOnSocket(fd)) {
        zsock_close(fd);
        setSocketError(QAbstractSocket::SslHandshakeFailedError);
        emit errorOccurred(QAbstractSocket::SslHandshakeFailedError);
        return;
    }

    setSocketDescriptor(fd, QAbstractSocket::UnconnectedState, openMode);
    QTcpSocket::connectToHost(hostName, port, openMode, QAbstractSocket::IPv4Protocol);

    connect(this, &QTcpSocket::connected, this, [this]() {
        printf("[tls] TLS connected\n");
        emit encrypted();
    }, Qt::SingleShotConnection);
}

bool QZephyrTlsSocket::configureTlsOnSocket(qintptr fd)
{
    int ret;

    if (!m_secTags.isEmpty()) {
        QList<sec_tag_t> tags;
        for (int t : m_secTags)
            tags.append(static_cast<sec_tag_t>(t));
        ret = zsock_setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST,
                               tags.constData(),
                               tags.size() * sizeof(sec_tag_t));
        if (ret < 0) {
            printf("[tls] TLS_SEC_TAG_LIST failed: %d\n", ret);
            return false;
        }
    }

    ret = zsock_setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY,
                           &m_peerVerify, sizeof(m_peerVerify));
    if (ret < 0) {
        printf("[tls] TLS_PEER_VERIFY failed: %d\n", ret);
        return false;
    }

    if (!m_tlsHostname.isEmpty()) {
        QByteArray host = m_tlsHostname.toUtf8();
        ret = zsock_setsockopt(fd, SOL_TLS, TLS_HOSTNAME,
                               host.constData(), host.size());
        if (ret < 0) {
            printf("[tls] TLS_HOSTNAME failed: %d\n", ret);
            return false;
        }
    }

    return true;
}
