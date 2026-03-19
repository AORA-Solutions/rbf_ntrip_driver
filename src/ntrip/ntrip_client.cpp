//
// Created by mehce on 16.06.2022.
//

#include "ntrip/ntrip_client.h"
#include <curl/curl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <poll.h>

#include <chrono>
#include <string>
#include <thread>  // NOLINT.
#include <list>
#include <vector>
#include <memory>

#include "ntrip/ntrip_util.h"
#include "ntrip/cmake_definition.h"


namespace libntrip {

    namespace {

// GPGGA format example.
        constexpr char gpgga_buffer[] =
                "$GPGGA,083552.00,3000.0000000,N,11900.0000000,E,"
                "1,08,1.0,0.000,M,100.000,M,,*57\r\n";

        constexpr int kBufferSize = 4096;

    }  // namespace

//
// Public method.
//

    static size_t CurlWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
    {
        auto *client = reinterpret_cast<NtripClient *>(userdata);
        size_t total = size * nmemb;
        if (client && total > 0) {
            client->ForwardToCallback(ptr, static_cast<int>(total));
        }
        return total;
    }

    bool NtripClient::Run(void) {
        if (service_is_running_.load()) return true;
        Stop();
        service_is_running_.store(true);
        thread_.reset(&NtripClient::ThreadHandler, this);
        return true;
    }

    void NtripClient::Stop(void) {
        service_is_running_.store(false);
        if (socket_fd_ > 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
        thread_.join();
    }

//
// Private method.
//

    void NtripClient::ThreadHandler(void) {
        CURL *curl = nullptr;
        CURLcode res;
        curl_global_init(CURL_GLOBAL_DEFAULT);
        while (service_is_running_.load()) {
            curl = curl_easy_init();
            if (!curl) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            std::string url = (use_https_ ? "https://" : "http://") + server_ip_ + ":" + std::to_string(server_port_) + "/" + mountpoint_;
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            std::string userpwd = user_ + ":" + passwd_;
            curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTP09_ALLOWED, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, kClientAgent);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_HTTP09_ALLOWED, 1L);
            // ask libcurl to only establish the connection (and TLS handshake for HTTPS)
            curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);

            res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                printf("libcurl connect error: %s\n", curl_easy_strerror(res));
                curl_easy_cleanup(curl);
                // small backoff before reconnecting
                for (int i = 0; i < 10 && service_is_running_.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                continue;
            }

            // connection established; get active socket
            curl_socket_t active_sock = CURL_SOCKET_BAD;
            curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &active_sock);
            if (active_sock == CURL_SOCKET_BAD) {
                curl_easy_cleanup(curl);
                continue;
            }

            // build request manually so we can then inject GGA sentences on the same connection
            std::string auth_raw = user_ + ":" + passwd_;
            std::string auth_b64;
            Base64Encode(auth_raw, &auth_b64);
            std::string request = "GET /" + mountpoint_ + " HTTP/1.0\r\n";
            request += "User-Agent: ";
            request += kClientAgent;
            request += "\r\n";
            request += "Ntrip-Version: Ntrip/2.0\r\n";
            request += "Authorization: Basic ";
            request += auth_b64;
            request += "\r\n\r\n";

            // send the initial GET request (use curl_easy_send for HTTPS, raw socket for HTTP)
            if (use_https_) {
                size_t sent = 0;
                res = curl_easy_send(curl, request.c_str(), request.size(), &sent);
                if (res != CURLE_OK) {
                    printf("curl_easy_send error: %s\n", curl_easy_strerror(res));
                    curl_easy_cleanup(curl);
                    continue;
                }
            } else {
                ssize_t s = send(active_sock, request.c_str(), request.size(), 0);
                if (s <= 0) {
                    curl_easy_cleanup(curl);
                    continue;
                }
            }

            // streaming loop: read data and periodically send GGA sentences
            char buffer[kBufferSize];
            auto last_gga = std::chrono::steady_clock::now();
            while (service_is_running_.load()) {
                // attempt to read any available data
                if (use_https_) {
                    size_t nread = 0;
                    res = curl_easy_recv(curl, buffer, kBufferSize, &nread);
                    if (res == CURLE_OK && nread > 0) {
                        callback_(buffer, static_cast<int>(nread));
                    } else if (res != CURLE_OK && res != CURLE_AGAIN) {
                        break; // connection problem
                    }
                } else {
                    struct pollfd pfd;
                    pfd.fd = active_sock;
                    pfd.events = POLLIN;
                    int pret = poll(&pfd, 1, 200);
                    if (pret > 0 && (pfd.revents & POLLIN)) {
                        ssize_t r = recv(active_sock, buffer, kBufferSize, 0);
                        if (r > 0) {
                            callback_(buffer, static_cast<int>(r));
                        } else {
                            break; // connection closed/error
                        }
                    }
                }

                // periodic GGA injection
                auto now = std::chrono::steady_clock::now();
                if (report_interval_ > 0 && std::chrono::duration_cast<std::chrono::seconds>(now - last_gga).count() >= report_interval_) {
                    std::string gga;
                    if (gga_is_update_.load()) {
                        gga = gga_buffer_;
                        gga_is_update_.store(false);
                    } else {
                        GGAFrameGenerate(latitude_, longitude_, 0.0, &gga);
                    }
                    if (!gga.empty()) {
                        if (use_https_) {
                            size_t sent = 0;
                            res = curl_easy_send(curl, gga.c_str(), gga.size(), &sent);
                            if (res != CURLE_OK) {
                                break;
                            }
                        } else {
                            ssize_t s = send(active_sock, gga.c_str(), gga.size(), 0);
                            if (s <= 0) break;
                        }
                        last_gga = now;
                    }
                }
            }

            // cleanup after streaming loop
            curl_easy_cleanup(curl);
            // small backoff before reconnecting
            for (int i = 0; i < 10 && service_is_running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        curl_global_cleanup();
    }

}  // namespace libntrip
