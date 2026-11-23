#pragma once

#include <QFont>
#include <QImage>
#include <QPageLayout>
#include <QStringList>
#include <QTextDocument>
#include <QUrl>
#include <functional>
#include <memory>
#include <vector>

class QPainter;
class QPrinter;

namespace Printing {

struct Options {
    QPageLayout pageLayout;
    QFont font;
    bool wideTablesToAppendix = true;
};

struct Section {
    // A stable 96-DPI metrics device; page geometry is converted from points.
    std::unique_ptr<QImage> metrics;
    std::unique_ptr<QTextDocument> document;
    QPageLayout layout;
    QString title;
    void drawPage(QPainter &painter, int page) const;
};

struct Job {
    std::vector<Section> sections;
    QStringList warnings;
    QString error;
    int pageCount() const;
    bool print(QPrinter &printer, QString *failure = nullptr) const;
};

using ImageResolver = std::function<QUrl(const QString &)>;
// Pure snapshot preparation: never edits/saves source or materializes attachments.
Job build(const QString &markdown, const Options &options, const ImageResolver &resolve);

} // namespace Printing
