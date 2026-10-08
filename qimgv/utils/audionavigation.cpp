#include "audionavigation.h"
#include <algorithm>

namespace AudioNavigation {

QString suffix(QStringView path) {
    qsizetype const nameStart = std::max(path.lastIndexOf(u'/', -1), path.lastIndexOf(u'\\', -1)) + 1;
    qsizetype const dot = path.lastIndexOf(u'.', -1);
    if(dot < nameStart)
        return {};
    return path.sliced(dot + 1).toString().toLower();
}

QStringList pool(QStringList const &files, QString const &current,
                 std::function<bool(QString const &suffix)> const &mayBeAudio) {
    QStringList result;
    for(auto const &file : files) {
        // The current file whatever its name: it is where the next step starts from.
        if(file == current) {
            result.append(file);
            continue;
        }
        QString const fileSuffix = suffix(file);
        if(!fileSuffix.isEmpty() && mayBeAudio(fileSuffix))
            result.append(file);
    }
    return result;
}

QString step(QStringList const &pool, QString const &current, int offset) {
    if(pool.isEmpty())
        return {};
    qsizetype const count = pool.count();
    qsizetype index = pool.indexOf(current);
    if(index == -1) {
        // Stepping from just outside either end of the pool.
        index = offset > 0 ? -1 : count;
    }
    qsizetype const target = ((index + offset) % count + count) % count;
    return pool.at(target);
}

QString pick(QStringList pool, QString const &current, std::function<QString(QStringList const &pool)> const &next,
             std::function<bool(QString const &path)> const &accept) {
    // Every turn either returns or removes a file, so this many turns are always enough.
    for(qsizetype turns = pool.count(); turns > 0; turns--) {
        QString const candidate = next(pool);
        if(candidate.isEmpty() || candidate == current || accept(candidate))
            return candidate;
        pool.removeAll(candidate);
    }
    return {};
}

} // namespace AudioNavigation
