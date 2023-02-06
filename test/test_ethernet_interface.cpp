#include "config_parser.hpp"
#include "ipaddress.hpp"
#include "mock_ethernet_interface.hpp"
#include "test_network_manager.hpp"

#include <net/if.h>

#include <sdbusplus/bus.hpp>
#include <stdplus/gtest/tmp.hpp>
#include <string_view>
#include <xyz/openbmc_project/Common/error.hpp>

#include <gtest/gtest.h>

namespace phosphor
{
namespace network
{

using sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument;
using std::literals::string_view_literals::operator""sv;
using testing::Key;
using testing::UnorderedElementsAre;

class TestEthernetInterface : public stdplus::gtest::TestWithTmp
{
  public:
    stdplus::Pinned<sdbusplus::bus_t> bus;
    std::filesystem::path confDir;
    TestManager manager;
    MockEthernetInterface interface;
    TestEthernetInterface() :
        bus(sdbusplus::bus::new_default()), confDir(CaseTmpDir()),
        manager(bus, "/xyz/openbmc_test/network", confDir),
        interface(makeInterface(bus, manager))

    {
    }

    static MockEthernetInterface
        makeInterface(stdplus::PinnedRef<sdbusplus::bus_t> bus,
                      TestManager& manager)
    {
        AllIntfInfo info{InterfaceInfo{.idx = 1, .flags = 0, .name = "test0"}};
        return {bus, manager, info, "/xyz/openbmc_test/network"sv,
                config::Parser()};
    }

    auto createIPObject(IP::Protocol addressType, const std::string& ipaddress,
                        uint8_t subnetMask)
    {
        return interface.ip(addressType, ipaddress, subnetMask, "");
    }

    auto createStaticRouteObject(std::string destination, std::string gateway,
                                 uint32_t prefixLength)
    {
        return interface.staticRoute(destination, gateway, prefixLength);
    }

    void setNtpServers()
    {
        ServerList ntpServers = {"10.1.1.1", "10.2.2.2", "10.3.3.3"};
        interface.EthernetInterfaceIntf::ntpServers(ntpServers);
    }

    ServerList getNtpServers()
    {
        return interface.EthernetInterfaceIntf::ntpServers();
    }
};

TEST_F(TestEthernetInterface, Fields)
{
    EXPECT_EQ(0, interface.mtu());
    EXPECT_EQ("", interface.macAddress());
    EXPECT_FALSE(interface.linkUp());

    constexpr ether_addr mac{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    constexpr unsigned mtu = 150;

    AllIntfInfo info{InterfaceInfo{.idx = 2,
                                   .flags = IFF_RUNNING,
                                   .name = "test1",
                                   .mac = mac,
                                   .mtu = mtu}};
    MockEthernetInterface intf(bus, manager, info,
                               "/xyz/openbmc_test/network"sv, config::Parser());

    EXPECT_EQ(mtu, intf.mtu());
    EXPECT_EQ(std::to_string(mac), intf.macAddress());
    EXPECT_TRUE(intf.linkUp());
}

TEST_F(TestEthernetInterface, NoIPaddress)
{
    EXPECT_TRUE(interface.addrs.empty());
}

TEST_F(TestEthernetInterface, AddIPAddress)
{
    EXPECT_THROW(createIPObject(IP::Protocol::IPv4, "127.0.0.1", 16),
                 InvalidArgument);
    EXPECT_THROW(createIPObject(IP::Protocol::IPv4, "127.0.0.1", 32),
                 InvalidArgument);
    EXPECT_THROW(createIPObject(IP::Protocol::IPv4, "192.168.1.1", 0),
                 InvalidArgument);
    EXPECT_THROW(createIPObject(IP::Protocol::IPv6, "::1", 64),
                 InvalidArgument);
    EXPECT_THROW(createIPObject(IP::Protocol::IPv6, "::", 128),
                 InvalidArgument);
    EXPECT_THROW(createIPObject(IP::Protocol::IPv6, "fe80::1", 0),
                 InvalidArgument);

    createIPObject(IP::Protocol::IPv4, "10.10.10.10", 16);
    EXPECT_THAT(interface.addrs, UnorderedElementsAre(Key(
                                     IfAddr(in_addr{htonl(0x0a0a0a0a)}, 16))));
}

TEST_F(TestEthernetInterface, AddMultipleAddress)
{
    createIPObject(IP::Protocol::IPv4, "10.10.10.10", 16);
    createIPObject(IP::Protocol::IPv4, "20.20.20.20", 16);
    EXPECT_THAT(
        interface.addrs,
        UnorderedElementsAre(Key(IfAddr(in_addr{htonl(0x0a0a0a0a)}, 16)),
                             Key(IfAddr(in_addr{htonl(0x14141414)}, 16))));
}

TEST_F(TestEthernetInterface, DeleteIPAddress)
{
    createIPObject(IP::Protocol::IPv4, "10.10.10.10", 16);
    createIPObject(IP::Protocol::IPv4, "20.20.20.20", 16);
    interface.addrs.at(IfAddr(in_addr{htonl(0x0a0a0a0a)}, 16))->delete_();
    EXPECT_THAT(interface.addrs, UnorderedElementsAre(Key(
                                     IfAddr(in_addr{htonl(0x14141414)}, 16))));
}

TEST_F(TestEthernetInterface, CheckObjectPath)
{
    auto path = createIPObject(IP::Protocol::IPv4, "10.10.10.10", 16);
    EXPECT_EQ(path.parent_path(), "/xyz/openbmc_test/network/test0");
    EXPECT_EQ(path.filename(), "10.10.10.10/16");
}

TEST_F(TestEthernetInterface, addStaticNameServers)
{
    ServerList servers = {"9.1.1.1", "9.2.2.2", "9.3.3.3"};
    EXPECT_CALL(manager.mockReload, schedule());
    interface.staticNameServers(servers);
    config::Parser parser((confDir / "00-bmc-test0.network").native());
    EXPECT_EQ(servers, parser.map.getValueStrings("Network", "DNS"));
}

TEST_F(TestEthernetInterface, getDynamicNameServers)
{
    ServerList servers = {"9.1.1.1", "9.2.2.2", "9.3.3.3"};
    EXPECT_CALL(interface, getNameServerFromResolvd())
        .WillRepeatedly(testing::Return(servers));
    EXPECT_EQ(interface.getNameServerFromResolvd(), servers);
}

TEST_F(TestEthernetInterface, addStaticNTPServers)
{
    ServerList servers = {"10.1.1.1", "10.2.2.2", "10.3.3.3"};
    EXPECT_CALL(manager.mockReload, schedule());
    interface.staticNTPServers(servers);
    config::Parser parser((confDir / "00-bmc-test0.network").native());
    EXPECT_EQ(servers, parser.map.getValueStrings("Network", "NTP"));
}

TEST_F(TestEthernetInterface, addNTPServers)
{
    using namespace sdbusplus::xyz::openbmc_project::Common::Error;
    ServerList servers = {"10.1.1.1", "10.2.2.2", "10.3.3.3"};
    EXPECT_THROW(interface.ntpServers(servers), NotAllowed);
}

TEST_F(TestEthernetInterface, getNTPServers)
{
    ServerList servers = {"10.1.1.1", "10.2.2.2", "10.3.3.3"};
    setNtpServers();
    EXPECT_EQ(getNtpServers(), servers);
}

TEST_F(TestEthernetInterface, addGateway)
{
    std::string gateway = "10.3.3.3";
    interface.defaultGateway(gateway);
    EXPECT_EQ(interface.defaultGateway(), gateway);
    EXPECT_THROW(interface.defaultGateway6("127.0.0.10"), InvalidArgument);
    EXPECT_EQ(interface.defaultGateway(), gateway);
    interface.defaultGateway("");
    EXPECT_EQ(interface.defaultGateway(), "");
}

TEST_F(TestEthernetInterface, addGateway6)
{
    std::string gateway6 = "fe80::1";
    interface.defaultGateway6(gateway6);
    EXPECT_EQ(interface.defaultGateway6(), gateway6);
    EXPECT_THROW(interface.defaultGateway6("::1"), InvalidArgument);
    EXPECT_EQ(interface.defaultGateway6(), gateway6);
    interface.defaultGateway6("");
    EXPECT_EQ(interface.defaultGateway6(), "");
}

TEST_F(TestEthernetInterface, DHCPEnabled)
{
    EXPECT_CALL(manager.mockReload, schedule())
        .WillRepeatedly(testing::Return());

    using DHCPConf = EthernetInterfaceIntf::DHCPConf;
    auto test = [&](DHCPConf conf, bool dhcp4, bool dhcp6) {
        EXPECT_EQ(conf, interface.dhcpEnabled());
        EXPECT_EQ(dhcp4, interface.dhcp4());
        EXPECT_EQ(dhcp6, interface.dhcp6());
    };
    test(DHCPConf::both, /*dhcp4=*/true, /*dhcp6=*/true);

    auto set_test = [&](DHCPConf conf, bool dhcp4, bool dhcp6) {
        EXPECT_EQ(conf, interface.dhcpEnabled(conf));
        test(conf, dhcp4, dhcp6);
    };
    set_test(DHCPConf::none, /*dhcp4=*/false, /*dhcp6=*/false);
    set_test(DHCPConf::v4, /*dhcp4=*/true, /*dhcp6=*/false);
    set_test(DHCPConf::v6, /*dhcp4=*/false, /*dhcp6=*/true);
    set_test(DHCPConf::both, /*dhcp4=*/true, /*dhcp6=*/true);

    auto ind_test = [&](DHCPConf conf, bool dhcp4, bool dhcp6) {
        EXPECT_EQ(dhcp4, interface.dhcp4(dhcp4));
        EXPECT_EQ(dhcp6, interface.dhcp6(dhcp6));
        test(conf, dhcp4, dhcp6);
    };
    ind_test(DHCPConf::none, /*dhcp4=*/false, /*dhcp6=*/false);
    ind_test(DHCPConf::v4, /*dhcp4=*/true, /*dhcp6=*/false);
    ind_test(DHCPConf::v6, /*dhcp4=*/false, /*dhcp6=*/true);
    ind_test(DHCPConf::both, /*dhcp4=*/true, /*dhcp6=*/true);
    set_test(DHCPConf::both, /*dhcp4=*/true, /*dhcp6=*/true);
}

TEST_F(TestEthernetInterface, IPv6AcceptRA)
{
    interface.ipv6AcceptRA(true);
    EXPECT_TRUE(interface.ipv6AcceptRA());
    interface.ipv6AcceptRA(false);
    EXPECT_FALSE(interface.ipv6AcceptRA());
}

TEST_F(TestEthernetInterface, AddStaticRoute)
{
    createStaticRouteObject("10.10.10.10", "10.10.10.1", 24);
    EXPECT_THAT(interface.staticRoutes,
                UnorderedElementsAre(Key(std::string("10.10.10.10"))));
}

TEST_F(TestEthernetInterface, AddMultipleStaticRoutes)
{
    createStaticRouteObject("10.10.10.10", "10.10.10.1", 24);
    createStaticRouteObject("10.20.30.10", "10.20.30.1", 24);
    EXPECT_THAT(interface.staticRoutes,
                UnorderedElementsAre(Key(std::string("10.10.10.10")),
                                     Key(std::string("10.20.30.10"))));
}

TEST_F(TestEthernetInterface, DeleteStaticRoute)
{
    createStaticRouteObject("10.10.10.10", "10.10.10.1", 24);
    createStaticRouteObject("10.20.30.10", "10.20.30.1", 24);

    interface.staticRoutes.at(std::string("10.10.10.10"))->delete_();
    interface.staticRoutes.at(std::string("10.20.30.10"))->delete_();
    EXPECT_EQ(interface.staticRoutes.empty(), true);
}

} // namespace network
} // namespace phosphor
