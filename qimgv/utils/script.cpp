#include "script.h"

Script::Script() : command(""), blocking(false) {}

Script::Script(QString _path, bool _blocking) : command(_path), blocking(_blocking) {}

QDataStream &operator<<(QDataStream &out, const Script &v) {
    out << v.command << v.blocking;
    return out;
}

QDataStream &operator>>(QDataStream &in, Script &v) {
    in >> v.command;
    in >> v.blocking;
    return in;
}
