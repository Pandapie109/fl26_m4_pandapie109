#include "aiws/gui/main_window.hpp"
#include "aiws/ipc.hpp"

#include <QApplication>
#include <QMessageBox>
#include <QString>
#include <QStringList>

#include <iostream>
#include <limits>

void confirm_exit(QWidget* parent) {
    const auto answer = QMessageBox::question(
        parent, "Exit AI Workspace", "Are you sure you want to exit?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::Yes) QApplication::quit();
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    QString socket_path = QString::fromStdString(aiws::ipc::default_socket_path());
    int delay = 0;

    for (int i = 1; i < args.size(); ++i) {
        if ((args[i] == "--ipc-socket" || args[i] == "--build-delay-ms") &&
            i + 1 < args.size()) {
            const QString key = args[i++];
            const QString value = args[i];
            if (key == "--ipc-socket") {
                socket_path = value;
            } else {
                bool ok = false;
                const int number = value.toInt(&ok);
                if (!ok || number < 0) {
                    std::cerr << "Invalid --build-delay-ms value\n";
                    return 1;
                }
                delay = number;
            }
        } else {
            std::cerr << "Usage: ai_workspace_m4 [--build-delay-ms N] [--ipc-socket PATH]\n";
            return 1;
        }
    }

    aiws::gui::MainWindow window;
    window.set_exit_callback(&confirm_exit);
    window.set_build_delay_ms(delay);
    QString error;
    if (!window.start_ipc_server(socket_path, &error)) {
        std::cerr << "Cannot start IPC receiver: " << error.toStdString() << '\n';
        return 1;
    }
    window.show();
    return app.exec();
}
