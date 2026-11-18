#pragma once

#include <QList>
#include <QString>

// A conservative, QtCore-only Markdown prose filter, not a Markdown parser.
class SpellingProse
{
public:
    struct Word {
        int start;
        int length;
        QString text;
    };

    // Source-relative UTF-16 offsets. Returned words are ordered, non-overlapping
    // and retain their original spelling (including internal apostrophes).
    static QList<Word> words(const QString &source);
};
