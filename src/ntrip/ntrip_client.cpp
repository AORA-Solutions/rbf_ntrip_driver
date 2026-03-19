//
// Created by mehce on 16.06.2022.
//

#include "ntrip/ntrip_client.h"

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

    bool NtripClient::Run(void) {
        if (service_is_running_.load()) return true;
        Stop();
        CURL *curl = curl_easy_init();
        if (!curl) {
            printf("Failed to initialize libcurl\n");
            return false;
        }
        std::string url = (use_https_ ? "https://" : "http://") + server_ip_ + ":" + std::to_string(server_port_) + "/" + mountpoint_;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERPWD, (user_ + ":" + passwd_).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTP09_ALLOWED, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "NTRIP ros2/ublox_dgnss");
        // TODO: Add GGA sending logic if required by caster
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("libcurl connection failed: %s\n", curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            return false;
        }
        // TODO: Setup data streaming and callbacks
        curl_easy_cleanup(curl);
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
        service_is_running_.store(true);
        int ret;
        std::unique_ptr<char[]> buffer(
                new char[kBufferSize], std::default_delete<char[]>());
        auto tp_beg = std::chrono::steady_clock::now();
        auto tp_end = tp_beg;
        int intv_ms = report_interval_ * 1000;
        while (service_is_running_.load()) {
            ret = recv(socket_fd_, buffer.get(), kBufferSize, 0);
            if (ret == 0) {
                break;
            } else if (ret < 0) {
                if ((errno != EAGAIN) && (errno != EWOULDBLOCK) && (errno != EINTR)) {
                    break;
                }
            } else {
                callback_(buffer.get(), ret);
                if (ret == kBufferSize) continue;
            }
            tp_end = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    tp_end-tp_beg).count() >= intv_ms) {
                tp_beg = std::chrono::steady_clock::now();
                if (!gga_is_update_.load()) {
                    GGAFrameGenerate(latitude_, longitude_, 10.0, &gga_buffer_);
                }
                send(socket_fd_, gga_buffer_.c_str(), gga_buffer_.size(), 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        service_is_running_.store(false);
    }

}  // namespace libntrip
