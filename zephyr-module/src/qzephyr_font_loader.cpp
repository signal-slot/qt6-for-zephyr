// Auto-register the embedded Roboto-Regular TTF as the default
// application font, so Qt examples that draw text work without
// modifying their main.cpp.
//
// Q_COREAPP_STARTUP_FUNCTION schedules the callback to run shortly
// after QCoreApplication / QGuiApplication has finished setting up
// the font database; QFontDatabase::addApplicationFontFromData is
// safe at that point.

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>
#include <QtGui/QFontDatabase>
#include <QtCore/QByteArray>

extern "C" unsigned char g_embedded_roboto_ttf[];
extern "C" unsigned int g_embedded_roboto_ttf_len;

namespace {

void registerEmbeddedRobotoFont()
{
    const QByteArray data(reinterpret_cast<const char *>(g_embedded_roboto_ttf),
                          int(g_embedded_roboto_ttf_len));
    const int fontId = QFontDatabase::addApplicationFontFromData(data);
    if (fontId < 0)
        return;
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    if (families.isEmpty())
        return;
    // Make Roboto the global application font so QML `font.family` etc.
    // default to a real face -- otherwise QPainter::drawText draws tofu.
    QGuiApplication::setFont(QFont(families.constFirst()));
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(registerEmbeddedRobotoFont)
