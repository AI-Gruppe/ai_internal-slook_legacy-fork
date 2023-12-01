#pragma once

#include "slook.hpp"

#if __has_include(<boost/asio.hpp>)
    #include <boost/asio.hpp>
#else
    #include <asio.hpp>
#endif

#include <memory>
#include <string>
#include <vector>

namespace slook {

#if __has_include(<boost/asio.hpp>)
using ec       = boost::system::error_code;
namespace asio = boost::asio;
#else
using ec = asio::error_code;
#endif

struct AsioServer : std::enable_shared_from_this<AsioServer> {
    template<typename T, std::size_t>
    using Vec = std::vector<T>;

    template<std::size_t S>
    using Str = std::string;

public:
    using Lookup_t = slook::Lookup<
      Vec,
      Str,
      std::function<void(std::optional<slook::IPAddress> const&, std::span<std::byte const>)>,
      std::function<void(slook::Service<Str, Vec> const&)>>;

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

    Lookup_t& getLookup() { return lookup; }

    void start() {
        try {
            resetTimer();
            initSocket();
            recv();
        } catch(std::exception const& e) {
            timer.cancel();
            fmt::print("slook: stopping now {}\n", e.what());
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
            fmt::print("slook: timeout restarting now\n");
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
        send_socket.set_option(asio::socket_base::reuse_address{true});
        send_socket.set_option(asio::socket_base::broadcast{true});
        send_socket.bind(send_ep);
        send_socket.set_option(asio::ip::multicast::join_group{mc_address});

        auto recv_ep = asio::ip::udp::endpoint(default_bind_address, port);

        recv_socket.open(recv_ep.protocol());
        recv_socket.set_option(asio::socket_base::reuse_address{true});
        recv_socket.set_option(asio::socket_base::broadcast{true});
        recv_socket.bind(recv_ep);
        recv_socket.set_option(asio::ip::multicast::join_group{mc_address});

        multicastSendEndpoint = asio::ip::udp::endpoint(mc_address, port);
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
                restart();
                fmt::print("slook: {}\n", error.message());
            }
        }
    }

    void handle_send(ec error) {
        sending = false;
        if(error) {
            if(error != asio::error::operation_aborted) {
                fmt::print("slook: {}\n", error.message());
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
}   // namespace slook
