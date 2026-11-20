#include "aiws/gui/main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTemporaryFile>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <spawn.h>
#include <signal.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include <stdexcept>

extern char** environ;

namespace {
struct SenderResult {
    bool started = false;
    bool timed_out = false;
    int exit_code = -1;
    QByteArray standard_error;
};

// Use the real sender executable without starting it through Qt.
// QProcess goes through Qt's process code, which crashes Clang TSan here.
SenderResult run_sender(const QString& binary, const QStringList& args,
                        const QString& directory = {}) {
    SenderResult result;
    QTemporaryFile error_file;
    if (!error_file.open()) return result;

    std::vector<QByteArray> strings;
    strings.push_back(QFile::encodeName(binary));
    for (const QString& arg : args) strings.push_back(arg.toUtf8());

    std::vector<char*> argv;
    for (auto& part : strings) argv.push_back(part.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) return result;
    int rc = ::posix_spawn_file_actions_adddup2(
        &actions, error_file.handle(), STDERR_FILENO);
    const QByteArray cwd = QFile::encodeName(directory);
    if (rc == 0 && !directory.isEmpty()) {
        rc = ::posix_spawn_file_actions_addchdir_np(&actions, cwd.constData());
    }

    pid_t pid = -1;
    if (rc == 0) {
        rc = ::posix_spawn(&pid, strings.front().constData(), &actions,
                           nullptr, argv.data(), environ);
    }
    ::posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return result;
    result.started = true;

    // Wait on the child with a deadline. Kill it if it gets stuck.
    std::promise<int> child_status;
    std::future<int> finished = child_status.get_future();
    std::thread waiter([pid, p = std::move(child_status)]() mutable {
        int status = -1;
        pid_t waited;
        do {
            waited = ::waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        p.set_value(waited == pid ? status : -1);
    });
    if (finished.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        result.timed_out = true;
        ::kill(pid, SIGKILL);
    }
    const int status = finished.get();
    waiter.join();
    if (!result.timed_out && status >= 0 && WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    }
    error_file.seek(0);
    result.standard_error = error_file.readAll();
    return result;
}

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
QPushButton* action_button(aiws::gui::MainWindow& w, const QString& label) {
    for (auto* button : w.findChildren<QPushButton*>()) {
        if (button->text() == label) return button;
    }
    return nullptr;
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
        QTRY_VERIFY_WITH_TIMEOUT(w.build_worker_active(), 1000);
        QVERIFY(w.add_document_file(second));
        QCOMPARE(corpus(w)->text(), QString("Not built"));
        QVERIFY(build_button(w)->isEnabled());
        w.set_build_delay_ms(0);
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(corpus(w)->text().endsWith(" chunks"), 1800);
        QCOMPARE(w.findChild<QListWidget*>("documentList")->count(), 2);
    }

    void reset_during_build_never_restores_stale_result() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = make_file(dir.path(), "first.txt", "some data for corpus");
        QVERIFY(!path.isEmpty());
        aiws::gui::MainWindow w;
        w.show();
        QVERIFY(w.add_document_file(path));
        w.set_build_delay_ms(400);
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(w.build_worker_active(), 1000);
        auto* reset = w.findChild<QPushButton*>("resetButton");
        QVERIFY(reset);
        QTest::mouseClick(reset, Qt::LeftButton);
        QCOMPARE(corpus(w)->text(), QString("Not built"));
        QCOMPARE(w.findChild<QListWidget*>("documentList")->count(), 0);
        QVERIFY(w.add_document_file(path));
        w.set_build_delay_ms(0);
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(corpus(w)->text().endsWith(" chunks"), 1800);
        QCOMPARE(w.findChild<QListWidget*>("documentList")->count(), 1);
    }

    void m5_cancel_a_long_build_before_starting_another() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        aiws::gui::MainWindow w;
        w.show();
        for (int i = 0; i < 6; ++i) {
            const QString path = make_file(dir.path(), QString("old%1.txt").arg(i),
                                           "old corpus text");
            QVERIFY(!path.isEmpty());
            QVERIFY(w.add_document_file(path));
        }
        w.set_build_delay_ms(350);
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(w.build_worker_active(), 1000);

        QTest::mouseClick(w.findChild<QPushButton*>("resetButton"), Qt::LeftButton);
        const QString new_file = make_file(dir.path(), "new.txt", "new corpus text");
        QVERIFY(w.add_document_file(new_file));
        w.set_build_delay_ms(0);
        QElapsedTimer elapsed;
        elapsed.start();
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        // Six old documents take over two seconds with cancellation broken.
        // The good build should finish after at most one old document delay.
        QTRY_VERIFY_WITH_TIMEOUT(corpus(w)->text().endsWith(" chunks"), 1400);
        QVERIFY2(elapsed.elapsed() < 1400, "A cancelled build held up the new build");
        QCOMPARE(w.findChild<QListWidget*>("documentList")->count(), 1);
    }

    void m5_remote_add_cancels_the_build() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString first = make_file(dir.path(), "first.txt", "original text");
        const QString second = make_file(dir.path(), "remote.txt", "remote text");
        QVERIFY(!first.isEmpty() && !second.isEmpty());

        aiws::gui::MainWindow w;
        w.show();
        QString error;
        const QString sock = dir.path() + "/during-build.sock";
        QVERIFY2(w.start_ipc_server(sock, &error), qPrintable(error));
        QVERIFY(w.add_document_file(first));
        w.set_build_delay_ms(450);
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(w.build_worker_active(), 1000);

        const QString binary = QCoreApplication::applicationDirPath() + "/aiws_send";
        const SenderResult sender = run_sender(binary, {"--socket", sock, "add", second});
        QVERIFY(sender.started);
        QVERIFY(!sender.timed_out);
        QCOMPARE(sender.exit_code, 0);
        auto* list = w.findChild<QListWidget*>("documentList");
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 2, 1200);
        QCOMPARE(corpus(w)->text(), QString("Not built"));
        QVERIFY(build_button(w)->isEnabled());
        w.set_build_delay_ms(0);
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(corpus(w)->text().endsWith(" chunks"), 1600);
    }

    void m5_exit_while_idle_joins_worker() {
        auto w = std::make_unique<aiws::gui::MainWindow>();
        QTRY_VERIFY_WITH_TIMEOUT(w->build_worker_waiting(), 1000);
        QElapsedTimer elapsed;
        elapsed.start();
        w.reset();
        QVERIFY2(elapsed.elapsed() < 700, "Idle worker did not stop on exit");
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

    void failed_rebuild_disables_processing_until_success() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = make_file(dir.path(), "current.txt", "alpha beta gamma");
        QVERIFY(!path.isEmpty());
        aiws::gui::MainWindow w;
        w.show();
        QVERIFY(w.add_document_file(path));

        auto* search = action_button(w, "Search");
        auto* inspect = action_button(w, "Inspect");
        auto* context = action_button(w, "Build Context");
        auto* query = w.findChild<QLineEdit*>();
        QVERIFY(search && inspect && context && query);

        query->setText("alpha");
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(corpus(w)->text().endsWith(" chunks"), 2500);
        QVERIFY(search->isEnabled());
        QVERIFY(inspect->isEnabled());
        QVERIFY(context->isEnabled());

        w.set_build_factory([]() -> std::unique_ptr<aiws::ProcessingCore> {
            throw std::runtime_error("intentional failed rebuild");
        });
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QVERIFY(!search->isEnabled());
        QVERIFY(!inspect->isEnabled());
        QVERIFY(!context->isEnabled());

        QTRY_VERIFY_WITH_TIMEOUT(status(w)->text().contains("Corpus build failed"), 2500);
        QCOMPARE(corpus(w)->text(), QString("Not built"));
        QVERIFY(!search->isEnabled());
        QVERIFY(!inspect->isEnabled());
        QVERIFY(!context->isEnabled());
        QVERIFY(build_button(w)->isEnabled());

        w.set_build_factory({});
        QTest::mouseClick(build_button(w), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(corpus(w)->text().endsWith(" chunks"), 2500);
        QVERIFY(search->isEnabled());
        QVERIFY(inspect->isEnabled());
        QVERIFY(context->isEnabled());
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

        const QString binary = QCoreApplication::applicationDirPath() + "/aiws_send";
        const SenderResult sender = run_sender(
            binary, {"--socket", sock, "add", filename}, dir.path());
        QVERIFY(sender.started);
        QVERIFY(!sender.timed_out);
        QCOMPARE(sender.exit_code, 0);
        auto* list = w.findChild<QListWidget*>("documentList");
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 1, 2000);

        const SenderResult missing = run_sender(
            binary, {"--socket", sock, "add", dir.path() + "/missing.txt"});
        QVERIFY(missing.started);
        QVERIFY(!missing.timed_out);
        QCOMPARE(missing.exit_code, 1);
        QVERIFY(!missing.standard_error.isEmpty());
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
        auto window = std::make_unique<aiws::gui::MainWindow>();
        QString error;
        QVERIFY2(window->start_ipc_server(sock, &error), qPrintable(error));
        for (int i = 0; i < 6; ++i) {
            const QString file = make_file(dir.path(), QString("shutdown%1.txt").arg(i), "words");
            QVERIFY(!file.isEmpty());
            QVERIFY(window->add_document_file(file));
        }
        window->set_build_delay_ms(1500);
        window->show();
        QTest::mouseClick(build_button(*window), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(window->build_worker_active(), 1000);
        QElapsedTimer timer;
        timer.start();
        window.reset();
        QVERIFY2(timer.elapsed() < 900, "Shutdown kept processing documents after Exit");
        QVERIFY(!QFileInfo::exists(sock));
    }
};

QTEST_MAIN(StudentTests)
#include "student_tests.moc"
