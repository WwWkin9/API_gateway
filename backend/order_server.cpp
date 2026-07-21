#include "backend_worker.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <iostream>
#include <string>

int main() {
    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9002);

    bind(fd, (sockaddr*)&addr, sizeof(addr));
    listen(fd, 1024);

    // 线程池：worker_count = 硬件并发数 × 2
    unsigned int hw = std::thread::hardware_concurrency();
    int workers = std::max(4u, hw * 2);

    BackendWorkerPool pool(workers, [](int cfd) {
        handle_client_keepalive(cfd, "order");
    });

    std::cout << "order server on 9002 (workers=" << workers << ", keep-alive)\n";

    while (true) {
        int cfd = accept(fd, nullptr, nullptr);
        if (cfd < 0) continue;
        pool.submit(cfd);
    }
}
