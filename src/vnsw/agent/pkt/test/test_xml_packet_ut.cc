#include <base/os.h>
#include <iostream>
#include <boost/program_options.hpp>
#include <testing/gunit.h>
#include <test/test_cmn_util.h>
#include "test-xml/test_xml.h"
#include "test-xml/test_xml_oper.h"
#include "oper/test/test_xml_physical_device.h"
#include "test_xml_flow_agent_init.h"

using namespace std;
namespace opt = boost::program_options;

static void GetArgs(char *test_file, int argc, char *argv[]) {
    test_file[0] = '\0';
    opt::options_description desc("Options");
    opt::variables_map vars;
    desc.add_options()
        ("help", "Print help message")
        ("test-data", opt::value<string>(), "Specify test data file");

    opt::store(opt::parse_command_line(argc, argv, desc), vars);
    opt::notify(vars);
    if (vars.count("test-data")) {
        strcpy(test_file, vars["test-data"].as<string>().c_str());
    }
    return;
}

class TestPkt : public ::testing::Test {
};

TEST_F(TestPkt, parse_1) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/pkt-parse.xml");
    AgentUtXmlOperInit(&test);
    if (test.Load() == true) {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
}

TEST_F(TestPkt, ingress_flow_1) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/ingress-flow.xml");
    AgentUtXmlOperInit(&test);
    if (test.Load() == true) {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
}

TEST_F(TestPkt, egress_flow_1) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/egress-flow.xml");
    AgentUtXmlOperInit(&test);
    if (test.Load() == true) {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
}

TEST_F(TestPkt, l2_sg_flow_1) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/l2-sg-flow.xml");
    AgentUtXmlOperInit(&test);
    if (test.Load() == true) {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
}

TEST_F(TestPkt, rpf_flow) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/rpf-flow.xml");
    AgentUtXmlOperInit(&test);
    if (test.Load() == true) {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
}

TEST_F(TestPkt, unknown_unicast_flood) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/unknown-unicast-flood.xml");
    AgentUtXmlOperInit(&test);
    if (test.Load() == true) {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
    client->WaitForIdle();
    client->agent()->flow_stats_collector()->set_delete_short_flow(true);
    client->EnqueueFlowAge();
    client->WaitForIdle();
    WAIT_FOR(000, 1000, (0U == client->agent()->pkt()->flow_table()->Size()));
    client->agent()->flow_stats_collector()->set_delete_short_flow(false);
}

TEST_F(TestPkt, flow_tsn_mode_1) {
    Agent *agent = Agent::GetInstance();
    //agent->set_tsn_enabled(true);
    IpamInfo ipam_info[] = {
        {"1.1.1.0", 24, "1.1.1.254", true}
    };
    AddVn("vn1", 1);
    AddVrf("vrf1", 1);
    AddIPAM("vn1", ipam_info, 1);
    client->WaitForIdle();

    boost::system::error_code ec;
    BgpPeer *bgp_peer = CreateBgpPeer(Ip4Address::from_string("0.0.0.1", ec),
                                      "xmpp channel");
    EcmpTunnelRouteAdd(agent, bgp_peer, "vrf1", "1.1.1.0", 24,
                       "100.100.100.1", 1, "100.100.100.2", 2, "vn1");

    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/tsn-flow.xml");
    AgentUtXmlOperInit(&test);
    AgentUtXmlPhysicalDeviceInit(&test);
    if (test.Load() == true) {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }

    DeleteRoute("vrf1", "1.1.1.0", 24, bgp_peer);
    client->WaitForIdle();
    DelIPAM("vn1");
    DelVn("vn1");
    client->WaitForIdle();
    DeleteBgpPeer(bgp_peer);
    client->WaitForIdle();
}

int main(int argc, char *argv[]) {
    GETUSERARGS();

    client = XmlPktParseTestInit(init_file, ksync_init);
    client->agent()->flow_stats_collector()->set_expiry_time(1000*1000);
    client->agent()->flow_stats_collector()->set_delete_short_flow(false);
    boost::system::error_code ec;
    bgp_peer_ = CreateBgpPeer(Ip4Address::from_string("0.0.0.1", ec),
                                          "xmpp channel");
    usleep(1000);
    client->WaitForIdle();
    int ret = RUN_ALL_TESTS();
    DeleteBgpPeer(bgp_peer_);
    TestShutdown();
    delete client;
    return ret;
}
