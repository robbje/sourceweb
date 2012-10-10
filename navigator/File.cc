#include "File.h"

#include <QString>
#include <QFile>
#include <QFileInfo>
#include <cstdlib>
#include <cstring>

namespace Nav {

File::File(Folder *parent, const QString &path) :
    m_parent(parent), m_path(path), m_loaded(false)
{
}

QString File::title()
{
    QFileInfo fi(m_path);
    return fi.fileName();
}

QString File::path()
{
    return m_path;
}

void File::loadFile()
{
    QFile qfile(m_path);
    if (!qfile.open(QFile::ReadOnly)) {
        m_content = "Error: cannot open " + m_path.toStdString();
    } else {
        // Read the file and canonicalize CRLF and CR line endings to LF.
        char *buf = new char[qfile.size() + 1];
        qfile.read(buf, qfile.size());
        buf[qfile.size()] = '\0';
        char *d = buf, *s = buf;
        while (*s != '\0') {
            if (*s == '\r') {
                *(d++) = '\n';
                ++s;
                if (*s == '\n')
                    ++s;
            } else {
                *(d++) = *(s++);
            }
        }
        m_content = std::string(buf, d - buf);
        delete [] buf;
    }

    // Identify where each line begins.
    const char *data = m_content.c_str();
    int lineStart = 0;
    for (int i = 0; ; i++) {
        if (data[i] == '\n') {
            m_lines.push_back(std::make_pair(lineStart, i - lineStart));
            lineStart = i + 1;
        } else if (data[i] == '\0') {
            if (i > lineStart)
                m_lines.push_back(std::make_pair(lineStart, i - lineStart));
            break;
        }
    }

    m_loaded = true;
}

} // namespace Nav
