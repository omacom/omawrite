#pragma once
#include <QString>
#include <QList>
#include <QPair>
class QTextDocument;
// Bake token formats into the imported Markdown document before HTML export.
// Return inclusive block-number groups as display-only metadata (HTML loses them).
QList<QPair<int, int>> highlightReadCode(QTextDocument &document, const QString &markdown, bool dark);
