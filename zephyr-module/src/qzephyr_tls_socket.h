#ifndef QZEPHYR_TLS_SOCKET_H
#define QZEPHYR_TLS_SOCKET_H

#include <QTcpSocket>
#include <QList>

class QZephyrTlsSocket : public QTcpSocket
{
    Q_OBJECT
public:
    explicit QZephyrTlsSocket(QObject *parent = nullptr);

    void setSecTags(const QList<int> &tags);
    void setPeerVerify(int mode);
    void setTlsHostname(const QString &hostname);

    void connectToHost(const QString &hostName, quint16 port,
                       OpenMode openMode = ReadWrite,
                       NetworkLayerProtocol protocol = AnyIPProtocol) override;

signals:
    void encrypted();

private:
    QList<int> m_secTags;
    int m_peerVerify = 2; // TLS_PEER_VERIFY_REQUIRED
    QString m_tlsHostname;

    bool configureTlsOnSocket(qintptr socketDescriptor);
};

#endif // QZEPHYR_TLS_SOCKET_H
