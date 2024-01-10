#pragma once

#include "slook.hpp"

#if __has_include(<boost/asio.hpp>)
    #include <boost/asio.hpp>
#else
    #include <asio.hpp>
#endif

#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace slook {

#if __has_include(<boost/asio.hpp>)
using ec       = boost::system::error_code;
namespace asio = boost::asio;
#else
using ec = asio::error_code;
#endif

template<typename Log>
struct AsioServer : std::enable_shared_from_this<AsioServer<Log>> {
    template<typename T, std::size_t>
    using Vec = std::vector<T>;

    template<std::size_t S>
    using Str = std::string;

private:
    using Lookup_t = slook::Lookup<
      Vec,
      Str,
      std::function<void(std::optional<slook::IPAddress> const&, std::span<std::byte const>)>,
      std::function<void(slook::Service<Str, Vec> const&)>>;

public:
    using Service = Lookup_t::Service;

    AsioServer(
      asio::io_context&                ioc_,
      std::uint16_t                    port_,
      std::string_view                 multicastAddress_,
      std::optional<asio::ip::address> interfaceAddress_ = std::nullopt)
      : ioc{ioc_}
      , send_socket{ioc}
      , recv_socket{ioc}
      , port{port_}
      , multicastAddress{std::move(multicastAddress_)}
      , interfaceAddress{interfaceAddress_}
      , timer{ioc}
      , lookup{
          [this](std::optional<slook::IPAddress> const& address, std::span<std::byte const> data) {
              send(address, data);
          }} {}

    void addService(Service const& new_service) { lookup.addService(new_service); }

    template<typename Cb>
    void findServices(std::string_view name, Cb&& cb) {
        lookup.findServices(name, std::forward<Cb>(cb));
    }

    void start() {
        try {
            resetTimer();
            initSocket();
            recv();
        } catch(std::exception const& e) {
            timer.cancel();
            Log{}("slook: stopping now {}", e.what());
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    asio::ip::udp::endpoint                                                 multicastSendEndpoint;
    std::vector<std::byte>                                                  recvData;
    bool                                                                    sending{false};
    std::vector<std::pair<asio::ip::udp::endpoint, std::vector<std::byte>>> openSendData;

    asio::ip::udp::endpoint lastRecvEndpoint;

    asio::io_service&                 ioc;
    asio::ip::udp::socket             send_socket;
    asio::ip::udp::socket             recv_socket;
    std::uint16_t                     port;
    std::string                       multicastAddress;
    std::optional<asio::ip::address>  interfaceAddress;
    asio::basic_waitable_timer<Clock> timer;
    Lookup_t                          lookup;

    static constexpr auto TimeoutInterval = std::chrono::seconds{15};

    void timeout(ec error) {
        if(error != asio::error::operation_aborted) {
            Log{}("slook: timeout restarting now");
            restart();
        }
    }

    void resetTimer() {
        timer.expires_at(Clock::now() + TimeoutInterval);
        timer.async_wait([self = this->shared_from_this()](auto error) { self->timeout(error); });
    }

    void initSocket() {
        auto const mc_address = asio::ip::make_address(multicastAddress);

#ifdef _WIN32
        auto const default_bind_address = asio::ip::udp::v4();
#else
        auto const default_bind_address = mc_address;
#endif
        auto const send_ep = [&]() {
            if(interfaceAddress) {
                return asio::ip::udp::endpoint(*interfaceAddress, port);
            } else {
                return asio::ip::udp::endpoint(default_bind_address, port);
            }
        }();

        send_socket.open(send_ep.protocol());
        if(interfaceAddress) {
            send_socket.set_option(
              asio::ip::multicast::outbound_interface(interfaceAddress->to_v4()));
        }
        send_socket.set_option(asio::ip::multicast::enable_loopback(true));

        auto recv_ep = asio::ip::udp::endpoint(default_bind_address, port);

        recv_socket.open(recv_ep.protocol());

        recv_socket.set_option(asio::socket_base::reuse_address{true});
        recv_socket.set_option(asio::socket_base::broadcast{true});
        if(interfaceAddress) {
            recv_socket.set_option(
              asio::ip::multicast::join_group{mc_address.to_v4(), interfaceAddress->to_v4()});
        } else {
            recv_socket.set_option(asio::ip::multicast::join_group{mc_address});
        }
        recv_socket.bind(recv_ep);

        multicastSendEndpoint = asio::ip::udp::endpoint(mc_address, port);

        Log{}("slook: starting on {}", send_ep.address().to_string());
    }

    void restart() {
        send_socket.cancel();
        send_socket = asio::ip::udp::socket{ioc};
        recv_socket.cancel();
        recv_socket = asio::ip::udp::socket{ioc};
        start();
    }

    void startSend() {
        sending = true;
        send_socket.async_send_to(
          asio::buffer(openSendData.front().second),
          openSendData.front().first,
          [self = this->shared_from_this()](auto error, auto) {
              self->openSendData.erase(self->openSendData.begin());
              self->handle_send(error);
          });
    }

    void handle_receive_from(ec error, std::size_t bytesRecvd) {
        if(!error) {
            slook::IPAddress address;
            if(lastRecvEndpoint.address().is_v4()) {
                auto add   = slook::IPv4Address{};
                auto bytes = lastRecvEndpoint.address().to_v4().to_bytes();
                std::transform(bytes.begin(), bytes.end(), add.begin(), [](auto x) {
                    return std::byte(x);
                });
                address = add;
            } else {
                auto add   = slook::IPv6Address{};
                auto bytes = lastRecvEndpoint.address().to_v6().to_bytes();
                std::transform(bytes.begin(), bytes.end(), add.begin(), [](auto x) {
                    return std::byte(x);
                });
                address = add;
            }
            lookup.messageCallback(
              address,
              std::span<std::byte const>{recvData.data(), bytesRecvd});
            resetTimer();
            recv();
        } else {
            if(error != asio::error::operation_aborted) {
                Log{}("slook: {}", error.message());
                restart();
            }
        }
    }

    void handle_send(ec error) {
        sending = false;
        if(error) {
            if(error != asio::error::operation_aborted) {
                Log{}("slook: {}", error.message());
                restart();
            }
            return;
        }

        if(!openSendData.empty()) {
            startSend();
        }
    }

    void recv() {
        recvData.resize(1024);
        recv_socket.async_receive_from(
          asio::buffer(recvData, 1024),
          lastRecvEndpoint,
          [self = this->shared_from_this()](auto error, auto s) {
              self->handle_receive_from(error, s);
          });
    }

    void send(std::optional<slook::IPAddress> const& address, std::span<std::byte const> data) {
        asio::ip::udp::endpoint ep;

        if(!address) {
            ep = multicastSendEndpoint;
        } else {
            if(std::holds_alternative<slook::IPv4Address>(*address)) {
                slook::IPv4Address const& slook_address = std::get<slook::IPv4Address>(*address);
                asio::ip::address_v4::bytes_type bytes;
                std::transform(
                  slook_address.begin(),
                  slook_address.end(),
                  bytes.begin(),
                  [](auto x) {
                      return static_cast<asio::ip::address_v4::bytes_type::value_type>(x);
                  });
                asio::ip::address_v4 boost_address{bytes};
                ep.address(boost_address);
            } else {
                slook::IPv6Address const& slook_address = std::get<slook::IPv6Address>(*address);
                asio::ip::address_v6::bytes_type bytes;
                std::transform(
                  slook_address.begin(),
                  slook_address.end(),
                  bytes.begin(),
                  [](auto x) {
                      return static_cast<asio::ip::address_v6::bytes_type::value_type>(x);
                  });
                asio::ip::address_v6 boost_address{bytes};
                ep.address(boost_address);
            }

            ep.port(multicastSendEndpoint.port());
        }

        std::vector<std::byte> d{};
        d.resize(data.size());
        std::copy(data.begin(), data.end(), d.begin());
        openSendData.emplace_back(ep, d);
        if(!sending) {
            startSend();
        }
    }
};

template<typename Log>
struct MultiInterfaceAsioServer : std::enable_shared_from_this<MultiInterfaceAsioServer<Log>> {
private:
    using Server_t = AsioServer<Log>;

public:
    using Service = Server_t::Service;

    MultiInterfaceAsioServer(
      asio::io_context& ioc_,
      std::uint16_t     port_,
      std::string_view  multicastAddress_)
      : ioc{ioc_}
      , port{port_}
      , multicastAddress{multicastAddress_}
      , resolveTimer{ioc}
      , resolver{ioc} {}

    void addService(Service const& new_service) {
        for(auto& server : servers) {
            auto s = server.second.lock();
            if(s) {
                s->addService(new_service);
            }
        }
        services.push_back(new_service);
    }

    template<typename Cb>
    void findServices(std::string_view name, Cb&& cb) {
        for(auto& server : servers) {
            auto s = server.second.lock();
            if(s) {
                s->findServices(name, cb);
            }
        }
    }

    void start() { resolveAndAdd(); }

    std::vector<std::string> interfaceIps() const {
        std::vector<std::string> ips;
        ips.reserve(servers.size());
        for(auto const& [ip, s] : servers) {
            ips.push_back(ip);
        }
        return ips;
    }

private:
    using Clock = std::chrono::steady_clock;
    asio::io_service&                              ioc;
    std::uint16_t                                  port;
    std::string                                    multicastAddress;
    asio::basic_waitable_timer<Clock>              resolveTimer;
    boost::asio::ip::tcp::resolver                 resolver;
    std::set<Service>                              services;
    std::map<std::string, std::weak_ptr<Server_t>> servers;

    static constexpr auto ResolveInterval = std::chrono::seconds{5};

    void resolveAndAdd() {
        boost::asio::ip::tcp::resolver::query query(boost::asio::ip::host_name(), "");

        resolver.async_resolve(query, [self = this->shared_from_this()](auto error, auto endpoints) {
            std::regex const ipv4Regex{
              R"(^(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$)"};

            std::vector<boost::asio::ip::address> interfaces{};
            if(error) {
                Log{}("slook error: resolve failed {}", error.message());
            } else {
                for(auto const& endpoint : endpoints) {
                    std::string ip = endpoint.endpoint().address().to_string();
                    if(
                      !endpoint.endpoint().address().is_loopback()
                      && endpoint.endpoint().address().is_v4() && std::regex_match(ip, ipv4Regex))
                    {
                        interfaces.push_back(endpoint.endpoint().address());
                    }
                }
            }

            for(auto const& interface : interfaces) {
                if(!self->servers.contains(interface.to_string())) {
                    auto slookServer = std::make_shared<Server_t>(
                      self->ioc,
                      self->port,
                      self->multicastAddress,
                      interface);
                    slookServer->start();
                    for(auto const& service : self->services) {
                        slookServer->addService(service);
                    }
                    self->servers[interface.to_string()] = slookServer;
                }
            }

            std::erase_if(self->servers, [](auto const& slookServer) {
                return slookServer.second.expired();
            });

            self->resolveTimer.expires_at(Clock::now() + ResolveInterval);
            self->resolveTimer.async_wait([innerSelf = self->shared_from_this()](auto ec1) {
                if(!ec1) {
                    innerSelf->resolveAndAdd();
                } else {
                    Log{}("slook error: {}", ec1.message());
                }
            });
        });
    }
};

}   // namespace slook
