#pragma once

#include <QDataStream>
#include <QMetaType>
#include <QString>

class Script {
public:
    Script();
    Script(QString _path, bool _blocking);
    QString command;
    bool blocking;
};

Q_DECLARE_METATYPE(Script)

// These must be visible to every translation unit that instantiates
// QMetaTypeForType<Script> -- which includes settings.cpp, where Script is
// round-tripped through QSettings. Declaring them only in main.cpp made
// serialization depend on link order: a TU that could not see them
// instantiated a metatype without stream operators, and whichever
// instantiation the linker happened to keep decided whether saved scripts
// survived a restart.
QDataStream &operator<<(QDataStream &out, const Script &v);
QDataStream &operator>>(QDataStream &in, Script &v);
