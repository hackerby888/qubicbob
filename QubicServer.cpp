#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <cstring>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>

#include "Logger.h"
#include "connection/connection.h"
#include "shim.h"
// Forward declaration from IOProcessor.cpp
void connReceiver(QCPtr& conn, const bool isTrustedNode, std::atomic_bool& stopFlag);

namespace {
    class QubicServer {
    public:
        static QubicServer& instance() {
            static QubicServer inst;
            return inst;
        }

        bool start(uint16_t port = 21842) {
            std::lock_guard<std::mutex> lk(m_);
            if (running_) return true;

            listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd_ < 0) {
                Logger::get()->critical("QubicServer: socket() failed (errno={})", errno);
                return false;
            }

            int yes = 1;
            ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
            ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(port);

            if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                Logger::get()->critical("QubicServer: bind() failed on port {} (errno={})", port, errno);
                ::close(listen_fd_);
                listen_fd_ = -1;
                return false;
            }

            if (::listen(listen_fd_, MAX_CONCURRENT_CONNECTIONS) < 0) {
                Logger::get()->critical("QubicServer: listen() failed (errno={})", errno);
                ::close(listen_fd_);
                listen_fd_ = -1;
                return false;
            }

            running_ = true;
            accept_thread_ = std::thread(&QubicServer::acceptLoop, this);
            cleanup_thread_ = std::thread(&QubicServer::cleanupThreadFunc, this);  // Start cleanup thread
            Logger::get()->info("QubicServer: listening on port {} (max {} connections, {} sec timeout)", 
                               port, MAX_CONCURRENT_CONNECTIONS, 2);
            return true;
        }

        void stop() {
            std::lock_guard<std::mutex> lk(m_);
            if (!running_) return;
            running_ = false;

            if (listen_fd_ >= 0) {
                ::shutdown(listen_fd_, SHUT_RDWR);
                ::close(listen_fd_);
                listen_fd_ = -1;
            }

            if (accept_thread_.joinable()) {
                accept_thread_.join();
            }
            
            if (cleanup_thread_.joinable()) {  // Join cleanup thread
                cleanup_thread_.join();
            }

            // Signal all client handlers to stop and disconnect sockets to break I/O.
            std::vector<std::shared_ptr<ClientCtx>> local_clients;
            {
                std::lock_guard<std::mutex> lk2(clients_m_);
                local_clients = clients_; // copy list to operate without holding the mutex
                for (auto& c : local_clients) {
                    c->stopFlag.store(true, std::memory_order_relaxed);
                    if (c->conn) {
                        c->conn->disconnect();
                    }
                    // Don't close ctx->fd - QubicConnection owns it
                }
            }

            // Join all client threads without holding clients_m_ to avoid deadlock
            for (auto& c : local_clients) {
                if (c->th.joinable()) c->th.join();
            }

            // Now clear the shared list
            {
                std::lock_guard<std::mutex> lk2(clients_m_);
                clients_.clear();
            }

            Logger::get()->info("QubicServer: stopped");
        }

    private:
        struct ClientCtx {
            std::atomic_bool stopFlag{false};
            QCPtr conn;
            std::thread th;
            int fd{-1};
            std::atomic_bool finished{false};  // Track when thread is done
        };

        QubicServer() = default;
        ~QubicServer() { stop(); }

        void cleanupFinishedClients() {
            std::lock_guard<std::mutex> lk(clients_m_);
            size_t before = clients_.size();
            clients_.erase(
                std::remove_if(clients_.begin(), clients_.end(),
                    [](const std::shared_ptr<ClientCtx>& ctx) {
                        if (ctx->finished.load(std::memory_order_acquire)) {
                            if (ctx->th.joinable()) {
                                ctx->th.join();
                            }
                            return true;  // Remove from vector
                        }
                        return false;
                    }),
                clients_.end()
            );
            size_t after = clients_.size();
            if (before != after) {
                Logger::get()->debug("QubicServer: Cleaned up {} finished client(s), {} active", 
                                    before - after, after);
            }
        }

        // New cleanup thread
        void cleanupThreadFunc() {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                cleanupFinishedClients();
            }
        }

        void acceptLoop() {
            // Add periodic cleanup counter
            int accept_count = 0;

            while (running_) {
                sockaddr_in cli{};
                socklen_t len = sizeof(cli);

                // Set accept timeout to allow periodic cleanup
                struct timeval tv;
                tv.tv_sec = 5;  // 5 second timeout
                tv.tv_usec = 0;
                ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &len);
                if (cfd < 0) {
                    if (!running_) break;
                    // On timeout or transient error, clean up finished clients
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        cleanupFinishedClients();
                        continue;
                    }
                    continue;
                }

                // Periodically clean up finished client threads
                if (++accept_count % 10 == 0) {
                    cleanupFinishedClients();
                }

                // Basic socket tuning
                int one = 1;
                ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_KEEPALIVE
                ::setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#endif

                auto ctx = std::make_shared<ClientCtx>();
                ctx->fd = cfd;

                // Wrap the accepted socket into QCPtr (NON-reconnectable as per connection.h)
                ctx->conn = make_qc_by_socket(cfd);

                {
                    std::lock_guard<std::mutex> lk(clients_m_);
                    // Check if we're at max capacity
                    if (clients_.size() >= MAX_CONCURRENT_CONNECTIONS) {
                        Logger::get()->warn("QubicServer: Max connections ({}) reached, rejecting new connection",
                                            MAX_CONCURRENT_CONNECTIONS);
                        ::close(cfd);
                        continue;
                    }
                    clients_.push_back(ctx);
                }

                // Non-trusted connections
                const bool isTrustedNode = false;

                // Launch per-connection receiver thread
                ctx->th = std::thread([this, ctx, isTrustedNode]() {
                    try {
                        ctx->conn->doHandshake();
                        connReceiver(ctx->conn, isTrustedNode, ctx->stopFlag);
                    } catch (const std::exception& ex) {
                        Logger::get()->warn("QubicServer: connReceiver exception for client: {}", ex.what());
                    } catch (...) {
                        Logger::get()->warn("QubicServer: connReceiver crashed for a client");
                    }

                    // Cleanup when receiver exits
                    if (ctx->conn) {
                        ctx->conn->disconnect();
                        ctx->conn.reset();
                    }
                    // Socket is now closed by QubicConnection, just mark as invalid
                    ctx->fd = -1;

                    // Mark as finished so acceptLoop can clean it up
                    ctx->finished.store(true, std::memory_order_release);
                });
            }

            // Final cleanup when exiting
            cleanupFinishedClients();
        }


    private:
        static constexpr size_t MAX_CONCURRENT_CONNECTIONS = 676;
        std::mutex m_;
        std::atomic_bool running_{false};
        int listen_fd_{-1};
        std::thread accept_thread_;
        std::thread cleanup_thread_;  // Add cleanup thread

        std::mutex clients_m_;
        std::vector<std::shared_ptr<ClientCtx>> clients_;
    };
} // namespace

// Public helpers to control the server
bool StartQubicServer(uint16_t port = 21842) {
    return QubicServer::instance().start(port);
}

void StopQubicServer() {
    QubicServer::instance().stop();
    Logger::get()->info("Stop qubic server");
}