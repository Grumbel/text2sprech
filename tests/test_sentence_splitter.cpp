#include <QtTest>

#include "document/SentenceSplitter.h"

class TestSentenceSplitter : public QObject {
    Q_OBJECT
private slots:
    void offsetsRoundTrip_data();
    void offsetsRoundTrip();
    void handlesAbbreviationsAndDecimals();
    void handlesInitials();
};

void TestSentenceSplitter::offsetsRoundTrip_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<int>("expectedCount");

    QTest::newRow("basic") << "Chapter 3. The spacecraft entered orbit around Mars. "
                              "The first images arrived twelve minutes later."
                           << 3;
    QTest::newRow("punctuation-mix") << "Is this it? Yes! Finally." << 3;
    QTest::newRow("empty") << "" << 0;
}

void TestSentenceSplitter::offsetsRoundTrip()
{
    QFETCH(QString, input);
    QFETCH(int, expectedCount);

    auto sentences = SentenceSplitter::split(input);
    QCOMPARE(sentences.size(), expectedCount);
    for (const auto &s : sentences) {
        // Every sentence's recorded offsets must round-trip back to its text --
        // this is what DocumentView relies on for QTextCursor placement.
        QCOMPARE(input.mid(s.start, s.end - s.start), s.text);
    }
}

void TestSentenceSplitter::handlesAbbreviationsAndDecimals()
{
    auto sentences = SentenceSplitter::split(
        "Dr. Smith went to Mars at 3.5 km/s. It was a long trip.");
    QCOMPARE(sentences.size(), 2);
    QCOMPARE(sentences[0].text, QStringLiteral("Dr. Smith went to Mars at 3.5 km/s."));
    QCOMPARE(sentences[1].text, QStringLiteral("It was a long trip."));
}

void TestSentenceSplitter::handlesInitials()
{
    auto sentences = SentenceSplitter::split("J. R. R. Tolkien wrote it. Everyone loved it.");
    QCOMPARE(sentences.size(), 2);
    QCOMPARE(sentences[0].text, QStringLiteral("J. R. R. Tolkien wrote it."));
}

QTEST_APPLESS_MAIN(TestSentenceSplitter)
#include "test_sentence_splitter.moc"
