#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <thread>

static void handle_client(int cfd) {
    std::string req;
    char buf[4096];
    ssize_t n;
    while ((n = recv(cfd, buf, sizeof(buf), 0)) > 0) {
        req.append(buf, buf + n);
        if (n < (ssize_t)sizeof(buf)) break;
    }

    auto pos = req.find("\r\n\r\n");
    std::string body = (pos != std::string::npos) ? req.substr(pos + 4) : "";

    std::string resp_body = std::string(R"({"service":"user","body":")") + body + R"("})";
    std::string resp =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
        std::to_string(resp_body.size()) + "\r\n\r\n" + resp_body;

    send(cfd, resp.c_str(), resp.size(), 0);
    close(cfd);
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9001);

    bind(fd, (sockaddr*)&addr, sizeof(addr));
    listen(fd, 128);

    std::cout << "user server on 9001 (multi-threaded)\n";

    while (true) {
        int cfd = accept(fd, nullptr, nullptr);
        std::thread(handle_client, cfd).detach();
    }
}
