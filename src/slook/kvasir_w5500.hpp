#pragma once

#include <kvasir/Devices/W5500.hpp>
#include <kvasir/Util/StaticFunction.hpp>
#include <kvasir/Util/StaticString.hpp>
#include <kvasir/Util/StaticVector.hpp>
#include <slook/slook.hpp>

namespace slook {
template<typename W5500, typename Clock>
struct W5500Server {
public:
    template<typename T, std::size_t S>
    using Vec = Kvasir::StaticVector<T, S>;

    template<std::size_t S>
    using Str = Kvasir::StaticString<S>;

    using Lookup_t = slook::Lookup<
      Vec,
      Str,
      Kvasir::StaticFunction<
        void(std::optional<slook::IPAddress> const& address, std::span<std::byte const>),
        16>,
      Kvasir::StaticFunction<void(slook::Service<Str, Vec> const&), 16>,
      1,
      0>;

    W5500Server(W5500& w5500_, std::uint16_t port_, ::IPAddress const& multicastAddress_)
      : port{port_}
      , multicastAddress{multicastAddress_}
      , w5500{w5500_}
      , lookup{
          [this](std::optional<slook::IPAddress> const& address,std::span<std::byte const> data) { send(address,data); },
        },lastMsg{Clock::now()} {}

    Lookup_t& getLookup() { return lookup; }

    bool valid() {
        auto const now = Clock::now();
        return !(now > (lastMsg + Timeout));
    }

    void handler() {
        if(!socket) {
            socket = w5500.udpMulticastSocket(port, multicastAddress);
            if(socket) {
                UC_LOG_D("valid socket");
            }
        } else {
            recvBuffer.resize(recvBuffer.max_size());
            auto const ret = socket->recv_from(recvBuffer);
            if(ret) {
                lastMsg = Clock::now();
                //UC_LOG_D("{}", *ret);
                slook::IPAddress address;
                address               = slook::IPv4Address{};
                slook::IPv4Address& a = std::get<slook::IPv4Address>(address);
                std::copy(
                  std::get<0>(*ret).octets.begin(),
                  std::get<0>(*ret).octets.end(),
                  a.begin());
                lookup.messageCallback(address, std::get<2>(*ret));
            }
        }
    }

    void send(std::optional<slook::IPAddress> const& address, std::span<std::byte const> data) {
        if(socket) {
            if(address) {
                ::IPAddress               a;
                slook::IPv4Address const& sa = std::get<slook::IPv4Address>(*address);
                std::copy(sa.begin(), sa.end(), a.octets.begin());
                [[maybe_unused]] auto const status = socket->send_to(a, port, data);
                UC_LOG_D("send uni {} status {}", address, status);
            } else {
                [[maybe_unused]] auto const status
                  = socket->send_multi(multicastAddress, port, data);
                UC_LOG_D("send multi status {}", status);
            }
        }
    }

private:
    static constexpr auto                    Timeout = std::chrono::seconds{5};
    std::uint16_t                            port;
    ::IPAddress                              multicastAddress;
    W5500&                                   w5500;
    Lookup_t                                 lookup;
    Kvasir::StaticVector<std::byte, 512>     recvBuffer;
    std::optional<typename W5500::UDPSocket> socket;
    Clock::time_point                        lastMsg;
};
}   // namespace slook
