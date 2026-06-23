// Import the wolfSSL TLS backend plugin so QSslSocket discovers it
// at runtime via the static plugin registry.

#include <QtCore/QtPlugin>

Q_IMPORT_PLUGIN(QTlsBackendWolfSSL)
