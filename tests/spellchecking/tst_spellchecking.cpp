#include "spellingcontroller.h"
#include "spellingprose.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QSemaphore>
#include <QThreadPool>
#include <QtConcurrentRun>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>
#include <QtTest>

namespace {
struct DictionaryState {
    QMap<QString, int> checks;
    QMap<QString, int> suggestions;
    QStringList selected;
    QStringList learned;
    QSet<QString> learnedWords;
    QSet<QString> unavailable;
    bool learnSucceeds = false;
    bool onApplicationThread = true;
    int totalChecks() const {
        int total = 0;
        for (int count : checks)
            total += count;
        return total;
    }
};

class FakeDictionary final : public SpellingDictionary {
public:
    explicit FakeDictionary(std::shared_ptr<DictionaryState> state) : m_state(state) {}
    QMap<QString, QString> dictionaries() const override {
        return {{QStringLiteral("English (US)"), QStringLiteral("en_US")},
                {QStringLiteral("English (UK)"), QStringLiteral("en_GB")}};
    }
    bool selectLanguage(const QString &language) override {
        recordThread();
        m_language = language;
        m_state->selected.append(language);
        return !m_state->unavailable.contains(language);
    }
    bool isCorrect(const QString &word) override {
        recordThread();
        ++m_state->checks[m_language + ":" + word];
        return word == QStringLiteral("good") || word == QStringLiteral("fixed")
            || (m_language == QStringLiteral("en_GB") && word == QStringLiteral("typo"))
            || m_state->learnedWords.contains(m_language + ":" + word);
    }
    QStringList suggestions(const QString &word) override {
        recordThread();
        ++m_state->suggestions[word];
        return {QStringLiteral("fixed"), word + QStringLiteral("-alternative")};
    }
    bool learn(const QString &word) override {
        recordThread();
        m_state->learned.append(word);
        if (m_state->learnSucceeds)
            m_state->learnedWords.insert(m_language + ":" + word);
        return m_state->learnSucceeds;
    }
private:
    void recordThread() {
        m_state->onApplicationThread &= QThread::currentThread() == QCoreApplication::instance()->thread();
    }
    std::shared_ptr<DictionaryState> m_state;
    QString m_language;
};

struct Fixture {
    std::shared_ptr<DictionaryState> state = std::make_shared<DictionaryState>();
    QTextDocument document;
    SpellingController controller{std::make_unique<FakeDictionary>(state)};
    Fixture() {
        // Never depend on the host locale or persisted production preferences.
        controller.setLanguage(QStringLiteral("en_US"));
        controller.attach(&document);
    }
    void setText(const QString &text) { document.setPlainText(text); }
};

QStringList texts(const QList<SpellingProse::Word> &words) {
    QStringList result;
    for (const auto &word : words)
        result.append(word.text);
    return result;
}
}

class SpellcheckingTest : public QObject {
    Q_OBJECT
private slots:
    void prose_data() {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("inline-code") << QString("before `hidden typo` after ``also ` hidden`` end")
            << QStringList{"before", "after", "end"};
        QTest::newRow("nested-containers")
            << QString("before\n> - ```cpp\n>   hidden typo\n>   ```\nafter")
            << QStringList{"before", "after"};
        QTest::newRow("ordered-nested-fence")
            << QString("before\n1. > ~~~python\n   > hidden typo\n   > ~~~\nafter")
            << QStringList{"before", "after"};
        QTest::newRow("literal-list-marker-inside-fence")
            << QString("before\n```md\n- ```\nhidden typo\n```\nafter")
            << QStringList{"before", "after"};
        QTest::newRow("literal-quote-marker-inside-fence")
            << QString("before\n```md\n> ```\nhidden typo\n```\nafter")
            << QStringList{"before", "after"};
        QTest::newRow("literal-list-inside-quoted-fence")
            << QString("before\n> - ```md\n>   - ```\n>   hidden typo\n>   ```\nafter")
            << QStringList{"before", "after"};
        QTest::newRow("indented-literal-fence")
            << QString("before\n```md\n    ```\nhidden typo\n```\nafter")
            << QStringList{"before", "after"};
        QTest::newRow("longer-outer-fence")
            << QString("before\n````md\n```\nhidden\n```\n````\nafter")
            << QStringList{"before", "after"};
        QTest::newRow("unclosed-fence") << QString("before\n```\nhidden typo")
            << QStringList{"before"};
        QTest::newRow("mermaid")
            << QString("before\n```mermaid\ngraph TD\nA[hidden] --> B[typo]\n```\nafter")
            << QStringList{"before", "after"};
        QTest::newRow("destinations")
            << QString("[visible label](https://host/path(nested) \"hidden title\") after")
            << QStringList{"visible", "label", "after"};
        QTest::newRow("reference-links")
            << QString("[visible][secret] [collapsed][]\n\n[secret]: https://host/path \"hidden\"\n[collapsed]: /hidden\n\nafter")
            << QStringList{"visible", "collapsed", "after"};
        QTest::newRow("multiline-reference-definition")
            << QString("before\n\n[secret]:\n  https://host/path\n  \"hidden title\"\n\nafter")
            << QStringList{"before", "after"};
        QTest::newRow("urls")
            << QString("before https://host/hidden www.example.org/path mailto:hidden@example.org <https://host/other> <hidden@example.org> after")
            << QStringList{"before", "after"};
        QTest::newRow("images")
            << QString("before ![hidden alt](image.png \"hidden title\") ![secret][imageid] ![shortcut] after\n\n[imageid]: /hidden.png")
            << QStringList{"before", "after"};
        QTest::newRow("html-entities")
            << QString("before <span title=\"hidden > typo\">visible</span> &amp; &#123; <!-- hidden --> after")
            << QStringList{"before", "visible", "after"};
        QTest::newRow("apostrophes-numbers") << QString::fromUtf8("can't l’esprit 123 abc123")
            << QStringList{QStringLiteral("can't"), QString::fromUtf8("l’esprit"), QStringLiteral("abc123")};
        QTest::newRow("indented-code")
            << QString("before\n\n    hidden typo\n    more hidden\n\nafter")
            << QStringList{"before", "after"};
        QTest::newRow("tab-code") << QString("\thidden typo\n\nafter") << QStringList{"after"};
        QTest::newRow("lazy-paragraph-indent")
            << QString("before\n    visible prose\nafter")
            << QStringList{"before", "visible", "prose", "after"};
        QTest::newRow("list-relative-indent")
            << QString("- visible\n\n    prose\n\n      hidden typo\n\nafter")
            << QStringList{"visible", "prose", "after"};
        QTest::newRow("nested-list-relative-indent")
            << QString("- outer\n  - inner\n\n      prose\n\n        hidden typo\n\nafter")
            << QStringList{"outer", "inner", "prose", "after"};
        QTest::newRow("quote-relative-indent")
            << QString("> visible\n>\n>     hidden typo\n\nafter")
            << QStringList{"visible", "after"};
        QTest::newRow("list-first-code")
            << QString("-     hidden typo\n\nafter") << QStringList{"after"};
        QTest::newRow("heading-then-code")
            << QString("# visible\n    hidden typo\n\nafter") << QStringList{"visible", "after"};
        QTest::newRow("thematic-break-code")
            << QString("***\n    hidden typo\n\nafter") << QStringList{"after"};
        QTest::newRow("spaced-thematic-break-code")
            << QString("> - - -\n>     hidden typo\n\nafter") << QStringList{"after"};
        QTest::newRow("empty") << QString() << QStringList{};
    }

    void prose() {
        QFETCH(QString, source);
        QFETCH(QStringList, expected);
        const auto words = SpellingProse::words(source);
        QCOMPARE(texts(words), expected);
        int end = 0;
        for (const auto &word : words) {
            QVERIFY(word.start >= end);
            QVERIFY(word.length > 0);
            QCOMPARE(source.mid(word.start, word.length), word.text);
            end = word.start + word.length;
        }
    }

    void unicodeSourceOffsets() {
        const QString source = QString::fromUtf8("😀 café é 𐐀word `hidden` naïve");
        const auto words = SpellingProse::words(source);
        QCOMPARE(texts(words), (QStringList{QString::fromUtf8("café"), QString::fromUtf8("é"),
                                           QString::fromUtf8("𐐀word"), QString::fromUtf8("naïve")}));
        const QList<int> starts{3, 8, 11, 27};
        const QList<int> lengths{4, 2, 6, 5};
        for (int i = 0; i < words.size(); ++i) {
            QCOMPARE(words[i].start, starts[i]);
            QCOMPARE(words[i].length, lengths[i]);
            QCOMPARE(source.mid(words[i].start, words[i].length), words[i].text);
        }
    }

    void debounceAndExactRanges() {
        Fixture f;
        f.setText("obsolete");
        QCOMPARE(f.state->totalChecks(), 0);
        QTest::qWait(50);
        f.setText(QString::fromUtf8("😀 good typo `hidden` typo"));
        QCOMPARE(f.state->totalChecks(), 0);
        QTest::qWait(50);
        QCOMPARE(f.state->totalChecks(), 0);
        QTRY_COMPARE(f.controller.ranges().size(), 2);
        QCOMPARE(f.controller.ranges().at(0).start, 8);
        QCOMPARE(f.controller.ranges().at(0).length, 4);
        QCOMPARE(f.controller.ranges().at(1).start, 22);
        QCOMPARE(f.controller.ranges().at(1).length, 4);
        QCOMPARE(f.state->checks.value("en_US:typo"), 1);
        QCOMPARE(f.state->checks.value("en_US:good"), 1);
        QCOMPARE(f.state->checks.value("en_US:obsolete"), 0);
        QCOMPARE(f.state->checks.value("en_US:hidden"), 0);
        QVERIFY(f.state->suggestions.isEmpty());
        QVERIFY(f.state->onApplicationThread);
    }

    void imeResumeWhileClassifierOutstandingDoesNotDuplicateRanges() {
        // Hold the only classifier worker so preedit can end after classify()
        // has launched but before its queued snapshot can complete.
        auto *pool = QThreadPool::globalInstance();
        const int previousMaximum = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        QSemaphore started, release;
        auto blocker = QtConcurrent::run([&] { started.release(); release.acquire(); });
        started.acquire();
        Fixture f;
        f.setText("typo");
        QTest::qWait(320);
        f.controller.setComposing(true);
        f.controller.setComposing(false);
        release.release();
        blocker.waitForFinished();
        pool->setMaxThreadCount(previousMaximum);
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        QTest::qWait(400); // exceed the redundant resume debounce
        QCOMPARE(f.controller.ranges().size(), 1);
        QCOMPARE(f.controller.ranges().first().start, 0);
        QCOMPARE(f.state->checks.value("en_US:typo"), 1);
    }

    void evictedTypoStillHasMenu() {
        Fixture f;
        QString text;
        for (int i = 0; i < 8300; ++i)
            text += QStringLiteral("typo%1 ").arg(i);
        f.setText(text);
        QTRY_COMPARE_WITH_TIMEOUT(f.controller.ranges().size(), 8300, 15000);
        const auto token = f.controller.wordAt(1);
        QCOMPARE(token.value("word").toString(), QStringLiteral("typo0"));
        QVERIFY(f.controller.validate(token));
        QCOMPARE(f.state->checks.value("en_US:typo0"), 1);
        QTRY_COMPARE(f.state->suggestions.value("typo0"), 1);
        QVERIFY(f.controller.ignore(token));
        QTRY_COMPARE(f.controller.ranges().size(), 8299);
    }

    void longUnmatchedBrackets() {
        const QString source = QStringLiteral("before ") + QString(100000, QLatin1Char('['))
            + QStringLiteral(" after");
        QCOMPARE(texts(SpellingProse::words(source)), (QStringList{"before", "after"}));
        const QString slashes = QStringLiteral("before ") + QString(100000, QLatin1Char('\\'))
            + QStringLiteral(" after");
        QCOMPARE(texts(SpellingProse::words(slashes)), (QStringList{"before", "after"}));
    }

    void cacheReusedAfterEdit() {
        Fixture f;
        f.setText("good typo typo");
        QTRY_COMPARE(f.controller.ranges().size(), 2);
        const int checks = f.state->totalChecks();
        f.setText("typo good typo typo");
        QVERIFY(f.controller.ranges().isEmpty());
        QTRY_COMPARE(f.controller.ranges().size(), 3);
        QCOMPARE(f.state->totalChecks(), checks);
    }

    void batchesPublishCompleteOrderedResults() {
        Fixture f;
        QStringList sourceWords;
        for (int i = 0; i < 150; ++i)
            sourceWords.append(QStringLiteral("typo%1").arg(i));
        const QString source = sourceWords.join(' ');
        QSignalSpy publications(&f.controller, &SpellingController::rangesChanged);
        f.setText(source);
        QTRY_COMPARE(f.controller.ranges().size(), sourceWords.size());
        int start = 0;
        for (int i = 0; i < sourceWords.size(); ++i) {
            QCOMPARE(f.controller.ranges().at(i).start, start);
            QCOMPARE(f.controller.ranges().at(i).length, sourceWords.at(i).size());
            start += sourceWords.at(i).size() + 1;
        }
        QCOMPARE(f.state->totalChecks(), 150);
        QTRY_COMPARE(publications.size(), 1); // Never publish growing prefixes.
        QTest::qWait(100);
        QCOMPARE(publications.size(), 1);
        QVERIFY(f.state->onApplicationThread);
        QVERIFY(f.state->suggestions.isEmpty());
    }

    void formattingDoesNotInvalidateSourceToken() {
        Fixture f;
        f.setText("typo");
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        const auto token = f.controller.wordAt(0);
        QTextCursor cursor(&f.document);
        cursor.select(QTextCursor::Document);
        QTextCharFormat format;
        format.setFontItalic(true);
        cursor.mergeCharFormat(format);
        QCOMPARE(f.document.toPlainText(), QString("typo"));
        QVERIFY(f.controller.validate(token));
        QCOMPARE(f.controller.ranges().size(), 1);
    }

    void staleTokensOnEditAndReattach() {
        Fixture f;
        f.setText("typo");
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        const auto token = f.controller.wordAt(1);
        QVERIFY(f.controller.validate(token));
        QTextCursor cursor(&f.document);
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(" good");
        QVERIFY(!f.controller.validate(token));
        QVERIFY(!f.controller.ignore(token));
        QVERIFY(!f.controller.learn(token));
        QVERIFY(f.state->learned.isEmpty());
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        const auto fresh = f.controller.wordAt(0);
        QVERIFY(f.controller.validate(fresh));
        QVERIFY(fresh.value("generation") != token.value("generation"));
        QTextDocument second("typo");
        f.controller.attach(&second);
        QVERIFY(!f.controller.validate(fresh));
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        f.controller.attach(nullptr);
        QVERIFY(f.controller.ranges().isEmpty());
        QVERIFY(f.controller.wordAt(0).isEmpty());
    }

    void languageSwitchInvalidatesCacheAndTokens() {
        Fixture f;
        f.setText("typo");
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        const auto token = f.controller.wordAt(0);
        f.controller.setLanguage("en_GB");
        QCOMPARE(f.controller.language(), QString("en_GB"));
        QVERIFY(f.controller.available());
        QVERIFY(!f.controller.validate(token));
        QTRY_COMPARE(f.state->checks.value("en_GB:typo"), 1);
        QVERIFY(f.controller.ranges().isEmpty());
        f.controller.setLanguage("en_US");
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        QCOMPARE(f.state->checks.value("en_US:typo"), 2);
        f.controller.setLanguage("not_installed");
        QCOMPARE(f.controller.language(), QString("en_US"));
        QVERIFY(f.controller.validate(f.controller.wordAt(0)));
    }

    void disabledStopsWorkAndInvalidatesTokens() {
        Fixture f;
        f.setText("typo");
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        const auto token = f.controller.wordAt(0);
        f.controller.setEnabled(false);
        QVERIFY(!f.controller.enabled());
        QVERIFY(f.controller.ranges().isEmpty());
        QVERIFY(!f.controller.validate(token));
        QVERIFY(f.controller.wordAt(0).isEmpty());
        const int checks = f.state->totalChecks();
        f.setText("other");
        QTest::qWait(350);
        QCOMPARE(f.state->totalChecks(), checks);
        f.controller.setEnabled(true);
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        QCOMPARE(f.controller.wordAt(0).value("word").toString(), QString("other"));
    }

    void missingDictionary() {
        QTextDocument document("typo");
        SpellingController absent{std::unique_ptr<SpellingDictionary>{}};
        absent.setLanguage("en_US");
        absent.attach(&document);
        QVERIFY(!absent.available());
        QVERIFY(absent.languages().isEmpty());
        QVERIFY(absent.wordAt(0).isEmpty());
        QVERIFY(!absent.validate({}));
        QTest::qWait(350);
        QVERIFY(absent.ranges().isEmpty());

        auto state = std::make_shared<DictionaryState>();
        state->unavailable = {"en_US", "en_GB"};
        SpellingController failed(std::make_unique<FakeDictionary>(state));
        failed.setLanguage("en_US");
        failed.attach(&document);
        QVERIFY(!failed.available());
        QCOMPARE(failed.languages().size(), 2);
        QTest::qWait(350);
        QCOMPARE(state->totalChecks(), 0);
        QVERIFY(failed.ranges().isEmpty());
    }

    void ignoreAcrossDocumentsAndLanguageScope() {
        Fixture f;
        f.setText("typo typo other");
        QTRY_COMPARE(f.controller.ranges().size(), 3);
        const auto token = f.controller.wordAt(0);
        QVERIFY(f.controller.ignore(token));
        QVERIFY(!f.controller.validate(token));
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        QCOMPARE(f.controller.wordAt(10).value("word").toString(), QString("other"));
        QTextDocument second("typo other");
        f.controller.attach(&second);
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        QCOMPARE(f.controller.ranges().first().start, 5);
        QVERIFY(f.state->learned.isEmpty());
        QVERIFY(f.controller.ignore(f.controller.wordAt(5)));
        QSignalSpy published(&f.controller, &SpellingController::rangesChanged);
        QTRY_VERIFY(!published.isEmpty());
        QVERIFY(f.controller.ranges().isEmpty());
        f.controller.setLanguage("en_GB");
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        QCOMPARE(f.controller.wordAt(5).value("word").toString(), QString("other"));
        // Ignore is not a personal-dictionary write and does not leak to a new controller.
        Fixture independent;
        independent.setText("typo");
        QTRY_COMPARE(independent.controller.ranges().size(), 1);
    }

    void learnFailureAndSuccess() {
        Fixture f;
        f.setText("typo typo");
        QTRY_COMPARE(f.controller.ranges().size(), 2);
        QSignalSpy failures(&f.controller, &SpellingController::operationFailed);
        const auto token = f.controller.wordAt(0);
        QVERIFY(!f.controller.learn(token));
        QCOMPARE(failures.size(), 1);
        QVERIFY(!failures.first().first().toString().isEmpty());
        QVERIFY(f.controller.validate(token));
        QCOMPARE(f.controller.ranges().size(), 2);
        f.state->learnSucceeds = true;
        QVERIFY(f.controller.learn(token));
        QVERIFY(!f.controller.validate(token));
        QTRY_COMPARE(f.state->checks.value("en_US:typo"), 2);
        QVERIFY(f.controller.ranges().isEmpty());
        QCOMPARE(f.state->learned, (QStringList{"typo", "typo"}));
        QCOMPARE(failures.size(), 1);
    }

    void suggestionsOnDemandAndCached() {
        Fixture f;
        f.setText("typo typo good");
        QTRY_COMPARE(f.controller.ranges().size(), 2);
        QVERIFY(f.state->suggestions.isEmpty());
        QVERIFY(f.controller.wordAt(-1).isEmpty());
        QVERIFY(f.controller.wordAt(11).isEmpty());
        const auto pending = f.controller.wordAt(0);
        QVERIFY(pending.value("pending").toBool());
        QVERIFY(pending.value("suggestions").toStringList().isEmpty());
        QCOMPARE(f.state->suggestions.value("typo"), 0); // Lookup itself is cache-only.
        f.controller.wordAt(5); // Duplicate request before the event loop runs.
        QSignalSpy menus(&f.controller, &SpellingController::menuChanged);
        QTRY_COMPARE(f.state->suggestions.value("typo"), 1);
        QVERIFY(!menus.isEmpty());
        const auto ready = f.controller.wordAt(5);
        QVERIFY(!ready.value("pending").toBool());
        QCOMPARE(ready.value("suggestions").toStringList(), (QStringList{"fixed", "typo-alternative"}));
        QVERIFY(f.controller.validate(pending));
        f.setText("typo good");
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        QVERIFY(!f.controller.wordAt(0).value("pending").toBool());
        QTest::qWait(30);
        QCOMPARE(f.state->suggestions.value("typo"), 1);
    }

    void queuedSuggestionsDiscardedOnEdit() {
        Fixture f;
        f.setText("typo");
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        QVERIFY(f.controller.wordAt(0).value("pending").toBool());
        f.setText("other");
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        QCOMPARE(f.state->suggestions.value("typo"), 0);
        QCOMPARE(f.controller.wordAt(0).value("word").toString(), QString("other"));
    }

    void imeDefersWorkAndMenus() {
        Fixture f;
        f.controller.setComposing(true);
        f.setText("typo");
        QTest::qWait(350);
        QCOMPARE(f.state->totalChecks(), 0);
        QVERIFY(f.controller.ranges().isEmpty());
        QVERIFY(f.controller.wordAt(0).isEmpty());
        f.controller.setComposing(false);
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        const auto token = f.controller.wordAt(0);
        f.controller.setComposing(true);
        QVERIFY(!f.controller.validate(token));
        QVERIFY(f.controller.wordAt(0).isEmpty());
        QSignalSpy ranges(&f.controller, &SpellingController::rangesChanged);
        f.setText("other");
        QTest::qWait(350);
        QCOMPARE(ranges.size(), 0);
        QCOMPARE(f.state->checks.value("en_US:other"), 0);
        f.controller.setComposing(false);
        QTRY_COMPARE(f.state->checks.value("en_US:other"), 1);
        QCOMPARE(f.controller.wordAt(0).value("word").toString(), QString("other"));
        QVERIFY(!f.controller.validate(token));
    }

    void replacementIsSourceOnlyAndOneUndo() {
        Fixture f;
        const QString source = QString::fromUtf8("😀 **typo** [good](https://host/typo) `typo`");
        f.setText(source);
        f.document.clearUndoRedoStacks();
        QTRY_COMPARE(f.controller.ranges().size(), 1);
        const auto token = f.controller.wordAt(source.indexOf("typo"));
        QVERIFY(f.controller.validate(token));
        QTextCursor cursor(&f.document);
        cursor.beginEditBlock();
        cursor.setPosition(token.value("start").toInt());
        cursor.setPosition(cursor.position() + token.value("length").toInt(), QTextCursor::KeepAnchor);
        cursor.insertText("fixed");
        cursor.endEditBlock();
        QString replaced = source;
        replaced.replace(source.indexOf("typo"), 4, "fixed");
        QCOMPARE(f.document.toPlainText(), replaced);
        QVERIFY(!f.controller.validate(token));
        QVERIFY(f.document.isUndoAvailable());
        f.document.undo();
        QCOMPARE(f.document.toPlainText(), source);
        QVERIFY(!f.document.isUndoAvailable());
        QVERIFY(!f.controller.validate(token)); // Restored text must not resurrect a stale generation.
        f.document.redo();
        QCOMPARE(f.document.toPlainText(), replaced);
    }
};

QTEST_GUILESS_MAIN(SpellcheckingTest)
#include "tst_spellchecking.moc"
