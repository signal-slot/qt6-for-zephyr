// SPDX-License-Identifier: Apache-2.0
// qsb2glsl: .qsb shader packs -> the GLSL ES 1.00 text Qt's GLES2 RHI
// backend passes to glShaderSource(), one file per variant.
//
// QRhiGles2 picks the "GLSL 100 es" entry of a QShader (Standard for
// fragment shaders, Standard or Batchable for vertex shaders: the scene
// graph's batch renderer uses the batchable variant for merged batches)
// and passes shader.shader() unchanged.  Reading the .qsb with QShader
// here reproduces those bytes exactly, which is what YakoGL's offline
// table needs (lookup by SHA-256 of the source).
//
// Output names: <stem>.vert / <stem>.frag for Standard, <stem>.batchable.vert
// for the batchable vertex variant, where <stem> is the .qsb basename
// without ".vert.qsb"/".frag.qsb".  Duplicate texts (a shader compiled
// into several modules) are written once; a name clash with different
// text gets a numeric suffix.

#include <QtCore/QCoreApplication>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QTextStream>
#include <rhi/qshader.h>

#include <cstdio>

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(data) == data.size();
}

#ifdef HAVE_QSBC
#include <QtQuick3DUtils/private/qqsbcollection_p.h>
#endif

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser p;
    p.setApplicationDescription("Extract Qt's GLSL ES 1.00 shader sources from .qsb files"
#ifdef HAVE_QSBC
                                " and Qt Quick 3D shadergen .qsbc collections"
#endif
                                );
    p.addHelpOption();
    p.addOption({{"o", "out"}, "Output directory", "dir", "."});
    p.addOption({"list", "Only list what would be written"});
    p.addPositionalArgument("paths", ".qsb files or directories to scan");
    p.process(app);

    const QString outDir = p.value("out");
    const bool listOnly = p.isSet("list");
    if (!listOnly && !QDir().mkpath(outDir)) {
        fprintf(stderr, "qsb2glsl: cannot create %s\n", qPrintable(outDir));
        return 1;
    }

    QStringList files;
    for (const QString &path : p.positionalArguments()) {
        if (QFileInfo(path).isDir()) {
            // Qt writes the packs into ".qsb/" directories: descend into hidden ones too.
            QDirIterator it(path, {"*.qsb", "*.qsbc"}, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
            while (it.hasNext())
                files << it.next();
        } else {
            files << path;
        }
    }
    files.sort();

    const QShaderKey keys[] = {
        QShaderKey(QShader::GlslShader, QShaderVersion(100, QShaderVersion::GlslEs), QShader::StandardShader),
        QShaderKey(QShader::GlslShader, QShaderVersion(100, QShaderVersion::GlslEs), QShader::BatchableVertexShader),
    };

    QHash<QByteArray, QString> written;   // sha256 -> file (dedup)
    QHash<QString, int> nameUse;
    QTextStream out(stdout);
    int count = 0, skipped = 0;
    // (file, stem, shader): .qsb files carry one shader; a shadergen .qsbc
    // collection carries a vertex + fragment pair per material/feature entry.
    struct Item { QString file; QString stem; QShader shader; };
    QList<Item> items;
    for (const QString &file : files) {
        if (file.endsWith(".qsbc")) {
#ifdef HAVE_QSBC
            QQsbIODeviceCollection coll(file);
            if (!coll.map(QQsbIODeviceCollection::Read)) {
                fprintf(stderr, "qsb2glsl: cannot map %s\n", qPrintable(file));
                continue;
            }
            const QString base = QFileInfo(file).completeBaseName();
            int n = 0;
            for (const QQsbCollection::Entry &e : coll.availableEntries()) {
                QQsbCollection::EntryDesc desc;
                if (!coll.extractEntry(e, desc))
                    continue;
                const QString stem = base + QString("_%1").arg(n++);
                items.append({file, stem, desc.vertShader});
                items.append({file, stem, desc.fragShader});
            }
            coll.unmap();
#else
            fprintf(stderr, "qsb2glsl: %s: built without Qt Quick 3D (no .qsbc support)\n", qPrintable(file));
#endif
            continue;
        }
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) {
            fprintf(stderr, "qsb2glsl: cannot read %s\n", qPrintable(file));
            continue;
        }
        const QShader shader = QShader::fromSerialized(f.readAll());
        if (!shader.isValid()) {
            fprintf(stderr, "qsb2glsl: %s is not a valid .qsb\n", qPrintable(file));
            continue;
        }
        QString stem = QFileInfo(file).fileName();
        if (stem.endsWith(".vert.qsb") || stem.endsWith(".frag.qsb")) stem.chop(9);
        else stem = QFileInfo(file).completeBaseName();
        items.append({file, stem, shader});
    }

    for (const Item &item : items) {
        const QString &file = item.file;
        const QString &stem = item.stem;
        const QShader &shader = item.shader;
        const QString ext = shader.stage() == QShader::VertexStage ? "vert" : "frag";
        if (!shader.isValid())
            continue;

        for (const QShaderKey &key : keys) {
            if (key.sourceVariant() == QShader::BatchableVertexShader && shader.stage() != QShader::VertexStage)
                continue;
            const QShaderCode code = shader.shader(key);
            if (code.shader().isEmpty()) {
                if (key.sourceVariant() == QShader::StandardShader) {
                    fprintf(stderr, "qsb2glsl: %s has no GLSL 100 es variant\n", qPrintable(file));
                    ++skipped;
                }
                continue;
            }
            const QByteArray text = code.shader();
            const QByteArray sha = QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex();
            if (written.contains(sha))
                continue;   // same text already extracted (shared shader)
            QString name = stem + (key.sourceVariant() == QShader::BatchableVertexShader ? ".batchable." : ".") + ext;
            const int n = nameUse[name]++;
            if (n > 0)
                name = stem + QString("_%1").arg(n) + (key.sourceVariant() == QShader::BatchableVertexShader ? ".batchable." : ".") + ext;
            written.insert(sha, name);
            out << sha << "  " << name << "  <- " << file << Qt::endl;
            if (!listOnly && !writeFile(outDir + "/" + name, text)) {
                fprintf(stderr, "qsb2glsl: cannot write %s/%s\n", qPrintable(outDir), qPrintable(name));
                return 1;
            }
            ++count;
        }
    }
    out << count << " shader sources" << (listOnly ? " (not written)" : "") << ", " << skipped << " .qsb without GLSL 100 es" << Qt::endl;
    return 0;
}
