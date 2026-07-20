// Minimal UDP echo client built on fuse's C++ wrapper (fuse/fuse.hpp),
// demonstrating usage from C++ alongside the plain-C echo_server example.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <fuse/fuse.hpp>

int main(int argc, char **argv) {
    uint16_t port = 9999;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    std::string message = argc > 2 ? argv[2] : "hello from fuse_echo_client";

    try {
        fuse::socket client("127.0.0.1", 0);
        std::vector<uint8_t> payload(message.begin(), message.end());

        client.send_to(payload, "127.0.0.1", port);
        auto reply = client.recv_from();

        std::cout << "server echoed " << reply.size()
                  << " bytes: " << std::string(reply.begin(), reply.end()) << "\n";
    } catch (const fuse::error &e) {
        std::cerr << "fuse error (status " << e.status() << "): " << e.what() << "\n";
        return 1;
    }

    return 0;
}
