#include "aiws/ipc.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string socket = aiws::ipc::default_socket_path();
    int position = 1;
    if (position < argc && std::string(argv[position]) == "--socket") {
        if (position + 1 >= argc) {
            std::cerr << "Usage: aiws_send [--socket PATH] add FILE\n";
            return 1;
        }
        socket = argv[position + 1];
        position += 2;
    }
    if (argc != position + 2 || std::string(argv[position]) != "add") {
        std::cerr << "Usage: aiws_send [--socket PATH] add FILE\n";
        return 1;
    }
    try {
        const std::string path = aiws::ipc::absolute_file_path(argv[position + 1]);
        std::string error;
        const int result = aiws::ipc::send_add(socket, path, error);
        if (result != 0) std::cerr << (error.empty() ? "Request was not accepted" : error) << '\n';
        return result;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
