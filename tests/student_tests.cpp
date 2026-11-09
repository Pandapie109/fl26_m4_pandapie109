#include "aiws/gui/main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace {
QString make_file(const QString& folder, const QString& name, const QByteArray& bytes) {
    const QString path = folder + "/" + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    if (file.write(bytes) != bytes.size()) return {};
    file.close();
    return path;
}

QLabel* corpus(aiws::gui::MainWindow& w) {
    return w.findChild<QLabel*>("corpusStatus");
}
QLabel* status(aiws::gui::MainWindow& w) {
    return w.findChild<QLabel*>("statusLabel");
}
QPushButton* build_button(aiws::gui::MainWindow& w) {
    return w.findChild<QPushButton*>("buildCorpusButton");
}
} // namespace

class StudentTests : public QObject {
    Q_OBJECT

private slots:
    void build_completes_and_becomes_searchable() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString first = make_file(dir.path(), "first.txt", "alpha beta gamma delta");
        QVERIFY(!first.isEmpty());
        aiws::gui::MainWindow w;
        w.show();
        QVERIFY(w.add_document_file(first));
        QVERIFY(build_button(w) && corpus(w) && status(w));
        w.set_build_delay_ms(60);
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QCOMPARE(corpus(w)->text(), QString("Building..."));
        QTRY_VERIFY_WITH_TIMEOUT(corpus(w)->text().endsWith(" chunks"), 3000);
        QVERIFY(status(w)->text().contains("Corpus built"));
    }

    void adding_while_building_never_accepts_old_corpus() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString first = make_file(dir.path(), "first.txt", "alpha beta gamma");
        const QString second = make_file(dir.path(), "second.txt", "delta epsilon zeta");
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        aiws::gui::MainWindow w;
        w.show();
        QVERIFY(w.add_document_file(first));
        w.set_build_delay_ms(450);
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QCOMPARE(corpus(w)->text(), QString("Building..."));
        QTest::qWait(40);
        QVERIFY(w.add_document_file(second));
        // Wait past the worker completion, not just the immediate invalidation.
        QTest::qWait(650);
        QCOMPARE(corpus(w)->text(), QString("Not built"));
        QVERIFY(build_button(w)->isEnabled());
        w.set_build_delay_ms(0);
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(corpus(w)->text().endsWith(" chunks"), 2500);
    }

    void reset_during_build_never_restores_stale_result() {
        QTemporaryDir dir;
        const QString path = make_file(dir.path(), "first.txt", "some data for corpus");
        QVERIFY(!path.isEmpty());
        aiws::gui::MainWindow w;
        w.show();
        QVERIFY(w.add_document_file(path));
        w.set_build_delay_ms(400);
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QTest::qWait(40);
        auto* reset = w.findChild<QPushButton*>("resetButton");
        QVERIFY(reset);
        QTest::mouseClick(reset, Qt::LeftButton);
        QCOMPARE(corpus(w)->text(), QString("Not built"));
        QTest::qWait(580);
        QCOMPARE(corpus(w)->text(), QString("Not built"));
        QCOMPARE(w.findChild<QListWidget*>("documentList")->count(), 0);
    }

    void failure_returns_to_recoverable_state() {
        QTemporaryDir dir;
        const QString path = make_file(dir.path(), "first.txt", "data");
        QVERIFY(!path.isEmpty());
        aiws::gui::MainWindow w;
        w.show();
        QVERIFY(w.add_document_file(path));
        w.set_build_factory([]() -> std::unique_ptr<aiws::ProcessingCore> {
            throw std::runtime_error("intentional test build failure");
        });
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(status(w)->text().contains("Corpus build failed"), 2000);
        QCOMPARE(corpus(w)->text(), QString("Not built"));
        QVERIFY(build_button(w)->isEnabled());
        w.set_build_factory({});
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(corpus(w)->text().endsWith(" chunks"), 2500);
    }

    void unreadable_path_has_no_effect() {
        aiws::gui::MainWindow w;
        QVERIFY(!w.add_document_file("/path/does/not/exist/for_m4.txt"));
        QCOMPARE(corpus(w)->text(), QString("Not built"));
        QCOMPARE(w.findChild<QListWidget*>("documentList")->count(), 0);
    }

    void remote_sender_accepts_utf8_newline_and_relative_paths() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString filename = QString::fromUtf8("unicode 日本 file\nsecond-line.txt");
        const QString path = make_file(dir.path(), filename, "a remote document with words");
        QVERIFY(!path.isEmpty());
        aiws::gui::MainWindow w;
        const QString sock = dir.path() + "/remote.sock";
        QString error;
        QVERIFY2(w.start_ipc_server(sock, &error), qPrintable(error));

        QProcess sender;
        sender.setWorkingDirectory(dir.path());
        const QString binary = QCoreApplication::applicationDirPath() + "/aiws_send";
        sender.start(binary, QStringList{"--socket", sock, "add", filename});
        QVERIFY(sender.waitForFinished(3000));
        QCOMPARE(sender.exitCode(), 0);
        auto* list = w.findChild<QListWidget*>("documentList");
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 1, 2000);

        QProcess missing;
        missing.start(binary, QStringList{"--socket", sock, "add", dir.path() + "/missing.txt"});
        QVERIFY(missing.waitForFinished(3000));
        QCOMPARE(missing.exitCode(), 1);
        QVERIFY(!missing.readAllStandardError().isEmpty());
        QCOMPARE(list->count(), 1);
    }

    void fragmented_remote_message_is_accepted() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = make_file(dir.path(), "partial.txt", "fragmented delivery test");
        QVERIFY(!path.isEmpty());
        aiws::gui::MainWindow w;
        const QString sock = dir.path() + "/partial.sock";
        QString error;
        QVERIFY2(w.start_ipc_server(sock, &error), qPrintable(error));
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(fd >= 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const QByteArray sock_bytes = sock.toUtf8();
        QVERIFY(static_cast<std::size_t>(sock_bytes.size()) < sizeof(addr.sun_path));
        std::memcpy(addr.sun_path, sock_bytes.constData(), static_cast<std::size_t>(sock_bytes.size()) + 1);
        QCOMPARE(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
        const QByteArray payload = path.toUtf8();
        const std::uint32_t net_size = htonl(static_cast<std::uint32_t>(payload.size()));
        const char* length_bytes = reinterpret_cast<const char*>(&net_size);
        QCOMPARE(::send(fd, length_bytes, 2, 0), static_cast<ssize_t>(2));
        QTest::qWait(35);
        QCOMPARE(::send(fd, length_bytes + 2, 2, 0), static_cast<ssize_t>(2));
        QTest::qWait(35);
        const int half = payload.size() / 2;
        QCOMPARE(::send(fd, payload.constData(), half, 0), static_cast<ssize_t>(half));
        QTest::qWait(35);
        QCOMPARE(::send(fd, payload.constData() + half, payload.size() - half, 0),
                 static_cast<ssize_t>(payload.size() - half));
        unsigned char result = 0xff;
        QCOMPARE(::recv(fd, &result, 1, 0), static_cast<ssize_t>(1));
        QCOMPARE(result, static_cast<unsigned char>(0));
        ::close(fd);
        auto* list = w.findChild<QListWidget*>("documentList");
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 1, 1500);
    }

    void shutdown_removes_socket_and_joins_worker() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sock = dir.path() + "/shutdown.sock";
        const QString file = make_file(dir.path(), "shutdown.txt", "words");
        auto window = std::make_unique<aiws::gui::MainWindow>();
        QString error;
        QVERIFY2(window->start_ipc_server(sock, &error), qPrintable(error));
        QVERIFY(window->add_document_file(file));
        window->set_build_delay_ms(1500);
        window->show();
        QTest::mouseClick(build_button(*window), Qt::LeftButton);
        QElapsedTimer timer;
        timer.start();
        window.reset();
        QVERIFY2(timer.elapsed() < 500, "Shutdown did not cancel/join the worker promptly");
        QVERIFY(!QFileInfo::exists(sock));
    }
};

QTEST_MAIN(StudentTests)
#include "student_tests.moc"
