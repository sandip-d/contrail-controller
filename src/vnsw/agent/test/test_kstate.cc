/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "test/test_init.h"
#include "test/test_cmn_util.h"
#include "test/test_kstate.h"
#include "oper/mirror_table.h"
#include "oper/tunnel_nh.h"
#include "xmpp/test/xmpp_test_util.h"
#include "ksync/ksync_sock_user.h"

#define vm1_ip "1.1.1.1"
#define vm2_ip "2.1.1.1"
#define vm3_ip "3.1.1.1"
#define vm4_ip "4.1.1.1"
#define vm5_ip "5.1.1.1"
#define SIP  0x01010101
#define DIP  0x01010102
#define SPORT 20130
#define DPORT 20131


#define MAX_VNET 1
int fd_table[MAX_VNET];
#define MAX_TEST_FD 5 
#define MAX_TEST_MPLS 10 
int test_fd[MAX_TEST_FD];

TestIfKState *TestIfKState::singleton_;
TestNHKState *TestNHKState::singleton_;
TestMplsKState *TestMplsKState::singleton_;
TestMirrorKState *TestMirrorKState::singleton_;
TestRouteKState *TestRouteKState::singleton_;
TestFlowKState *TestFlowKState::singleton_;
int TestKStateBase::handler_count_;
int TestKStateBase::fetched_count_;

struct PortInfo input[] = {
    {"test0", 5, vm1_ip, "00:00:00:01:01:01", 3, 1},
    {"test1", 6, vm2_ip, "00:00:00:02:02:02", 3, 2},
    {"test2", 7, vm3_ip, "00:00:00:01:01:03", 3, 3},
    {"test3", 8, vm4_ip, "00:00:00:02:02:04", 3, 4},
    {"test4", 9, vm5_ip, "00:00:00:02:02:05", 3, 5},
};

std::string analyzer = "TestAnalyzer";

void RouterIdDepInit() {
}

class KStateTest : public ::testing::Test {
public:

    static void TestSetup(bool ksync_init) {
        ksync_init_ = ksync_init;
        //Physical interface name specified in default config file is vnet0.
        //Create this if it is in ksync_init mode
        if (ksync_init_) {
            CreateTapInterfaces("vnet", MAX_VNET, fd_table);
            client->WaitForIdle(2);

            CreateTapInterfaces("test", MAX_TEST_FD, test_fd);
            client->WaitForIdle(2);
        }
        //To disable flow aging set the flow age time to high value
        AgentUve::GetInstance()->GetFlowStatsCollector()->
                                 SetFlowAgeTime(1000000 * 60 * 10);

        client->SetFlowFlushExclusionPolicy();
        VxLanNetworkIdentifierMode(false);
        client->WaitForIdle();
    }

    static void TestTearDown() {
        if (ksync_init_) {
            DeleteTapIntf(fd_table, MAX_VNET);
            client->WaitForIdle(2);
            DeleteTapIntf(test_fd, MAX_TEST_FD);
            client->WaitForIdle(2);
        }
    }
    
    void WaitForVrf(struct PortInfo *input, int idx, bool created) {
        char vrf_name[80];
        sprintf(vrf_name, "vrf%d", input[idx].vn_id);
        WAIT_FOR(1000, 1000, (VrfFind(vrf_name) == created));
    }

    void CreateVmPorts(struct PortInfo *input, int count) {
        CreateVmportEnv(input, count);
    }

    void CreatePorts(int if_count, int nh_count, int rt_count) {
        int idx;
        client->Reset();
        CreateVmPorts(input, MAX_TEST_FD);
        client->WaitForIdle(2);

        for (int i = 0; i < MAX_TEST_FD; i++) {
            idx = i;
            WAIT_FOR(1000, 1000, (VmPortActive(input, idx) == true));
        }
        WAIT_FOR(1000, 1000, (MAX_TEST_FD == Agent::GetInstance()->GetVmTable()->Size()));
        WAIT_FOR(1000, 1000, (1 == Agent::GetInstance()->GetVnTable()->Size()));
        WaitForVrf(input, 0, true);
        if (if_count) {
	    unsigned int oper_if_count = MAX_TEST_FD + if_count;
            WAIT_FOR(1000, 1000, ((oper_if_count) == 
                                Agent::GetInstance()->GetInterfaceTable()->Size()));
        }
        WAIT_FOR(1000, 1000, (MAX_TEST_MPLS == 
                            Agent::GetInstance()->GetMplsTable()->Size()));
        if (!ksync_init_) {
            WAIT_FOR(1000, 1000, (MAX_TEST_MPLS == 
                                 KSyncSockTypeMap::MplsCount()));
            if (if_count) {
                WAIT_FOR(1000, 1000, ((MAX_TEST_FD + if_count) == 
                                    KSyncSockTypeMap::IfCount()));
            }
            if (nh_count) {
                WAIT_FOR(1000, 1000, ((nh_count + (MAX_TEST_FD * 6) - 1) == 
                                    KSyncSockTypeMap::NHCount()));
            }
            if (rt_count) {
                WAIT_FOR(1000, 1000, ((rt_count + (MAX_TEST_FD * 2) + 1) == 
                                    KSyncSockTypeMap::RouteCount()));
            }
        }
    }

    void DeletePorts() {
        DeleteVmportEnv(input, MAX_TEST_FD, true);
        client->WaitForIdle(2);
        WAIT_FOR(1000, 1000, (0 == Agent::GetInstance()->GetVmTable()->Size()));
        WAIT_FOR(1000, 1000, (0 == Agent::GetInstance()->GetVnTable()->Size()));
        WaitForVrf(input, 0, false);
        WAIT_FOR(1000, 1000, (0 == Agent::GetInstance()->GetMplsTable()->Size()));
    }
    
    void CreatePortsWithPolicy() {
        int idx;
        client->Reset();
        CreateVmportEnv(input, MAX_TEST_FD, 1);
        client->WaitForIdle(2);

        for (int i = 0; i < MAX_TEST_FD; i++) {
            idx = i;
            WAIT_FOR(1000, 1000, (VmPortActive(input, idx) == true));
            WAIT_FOR(1000, 1000, (VmPortPolicyEnable(input, idx) == true));
        }
        WAIT_FOR(1000, 1000, (MAX_TEST_FD == Agent::GetInstance()->GetVmTable()->Size()));
        WAIT_FOR(1000, 1000, (1 == Agent::GetInstance()->GetVnTable()->Size()));
        WaitForVrf(input, 0, true);
        WAIT_FOR(1000, 1000, ((MAX_TEST_FD + 3) == Agent::GetInstance()->GetInterfaceTable()->Size()));
        if (!ksync_init_) {
            WAIT_FOR(1000, 1000, ((MAX_TEST_FD + 3) == KSyncSockTypeMap::IfCount()));
        }
    }

    void DeletePortsWithPolicy() {
        int idx;
        client->Reset();
        DeleteVmportEnv(input, MAX_TEST_FD, true, 1);
        client->PortDelNotifyWait(MAX_TEST_FD);
        client->WaitForIdle(2);
        for (int i = 0; i < MAX_TEST_FD; i++) {
            idx = i;
            WAIT_FOR(1000, 1000, (VmPortFind(input, idx) == false));
        }
        WAIT_FOR(1000, 1000, (0 == Agent::GetInstance()->GetVmTable()->Size()));
        WAIT_FOR(1000, 1000, (0 == Agent::GetInstance()->GetVnTable()->Size()));
        WaitForVrf(input, 0, false);
        WAIT_FOR(1000, 1000, (3 == Agent::GetInstance()->GetInterfaceTable()->Size()));
    }

    void CreateMirrorEntry() {
        Ip4Address sip(SIP);
        Ip4Address dip(DIP);

        //Create Mirror entry
        MirrorTable::AddMirrorEntry(analyzer, Agent::GetInstance()->GetDefaultVrf(),
                                    sip, SPORT, dip, DPORT);
        client->WaitForIdle(2);

        //Verify mirror NH is created
        MirrorNHKey key(Agent::GetInstance()->GetDefaultVrf(), sip, SPORT, dip, DPORT);
        WAIT_FOR(1000, 1000, (Agent::GetInstance()->GetNextHopTable()->FindActiveEntry(&key) != NULL));
    }

    void DeleteMirrorEntry() {
        Ip4Address sip(SIP);
        Ip4Address dip(DIP);

        MirrorTable::DelMirrorEntry(analyzer);
        client->WaitForIdle(2);

        //Verify mirror NH is deleted
        MirrorNHKey key(Agent::GetInstance()->GetDefaultVrf(), sip, SPORT, dip, DPORT);
        WAIT_FOR(1000, 1000, (Agent::GetInstance()->GetNextHopTable()->FindActiveEntry(&key) == NULL));
    }

    static bool ksync_init_;
};

bool KStateTest::ksync_init_;

TEST_F(KStateTest, IfDumpTest) {
    int if_count = 0;
    TestIfKState::Init();
    client->KStateResponseWait(1);
    if_count = TestKStateBase::fetched_count_;
    LOG(DEBUG, "if count " << if_count);
    
    CreatePorts(if_count, 0, 0);
    TestIfKState::Init(-1, true, if_count + MAX_TEST_FD);
    client->KStateResponseWait(1);
    DeletePorts();
}

TEST_F(KStateTest, IfGetTest) {
    int if_count = 0;
    TestIfKState::Init();
    client->KStateResponseWait(1);
    if_count = TestKStateBase::fetched_count_;
    LOG(DEBUG, "if count " << if_count);
    
    CreatePorts(if_count, 0, 0);
    for (int i = 0; i < MAX_TEST_FD; i++) {
        TestIfKState::Init(if_count + i);
        client->KStateResponseWait(1);
    }
    DeletePorts();
}

TEST_F(KStateTest, NHDumpTest) {
    int nh_count = 0;
    TestNHKState::Init();
    client->KStateResponseWait(1);
    nh_count = TestKStateBase::fetched_count_;
    LOG(DEBUG, "nh count " << nh_count);

    CreatePorts(0, nh_count, 0);
    //Two interface nexthops get created for each interface (with and without policy)
    //plus l2 without policy
    TestNHKState::Init(-1, true, nh_count + (MAX_TEST_FD * 3) + 3);
    client->KStateResponseWait(1);

    DeletePorts();
}

TEST_F(KStateTest, NHGetTest) {
    int nh_count = 0;
    TestNHKState::Init();
    client->KStateResponseWait(1);
    nh_count = TestKStateBase::fetched_count_;
    LOG(DEBUG, "nh count " << nh_count);

    CreatePorts(0, nh_count, 0);
    //Two interface nexthops get created for each interface (with and without policy)
    for (int i = 0; i < (MAX_TEST_FD * 2); i++) {
        TestNHKState::Init(nh_count + i);
        client->KStateResponseWait(1);
    }
    DeletePorts();
}

TEST_F(KStateTest, MplsDumpTest) {
    int mpls_count = 0;
    TestMplsKState::Init();
    client->KStateResponseWait(1);
    mpls_count = TestKStateBase::fetched_count_;
    
    CreatePorts(0, 0, 0);
    TestMplsKState::Init(-1, true, mpls_count + MAX_TEST_MPLS);
    client->KStateResponseWait(1);

    DeletePorts();
}

TEST_F(KStateTest, MplsGetTest) {
    int mpls_count = 0;
    TestMplsKState::Init();
    client->KStateResponseWait(1);
    mpls_count = TestKStateBase::fetched_count_;

    CreatePorts(0, 0, 0);
    for (int i = 0; i < MAX_TEST_FD; i++) {
        TestMplsKState::Init(MplsTable::kStartLabel + mpls_count + i);
        client->KStateResponseWait(1);
    }

    DeletePorts();
}

TEST_F(KStateTest, MirrorNHGetTest) {
    unsigned int nh_count = 0;
    TestNHKState::Init();
    client->KStateResponseWait(1);
    nh_count = TestKStateBase::fetched_count_;
    LOG(DEBUG, "nh count " << nh_count);

    //Create mirror entry
    CreateMirrorEntry();

    //Verify that NH table size is increased by 1
    EXPECT_EQ(Agent::GetInstance()->GetNextHopTable()->Size(), nh_count + 1);

    //Verify the get of Mirror NH
    TestNHKState::Init(nh_count);
    client->KStateResponseWait(1);

    DeleteMirrorEntry();
}

TEST_F(KStateTest, MirrorDumpTest) {
    int mirror_count = 0;
    TestMirrorKState::Init();
    client->KStateResponseWait(1);
    mirror_count = TestKStateBase::fetched_count_;

    CreateMirrorEntry();
    TestMirrorKState::Init(-1, true, mirror_count + 1);
    client->KStateResponseWait(1);
    DeleteMirrorEntry();
}

TEST_F(KStateTest, MirrorGetTest) {
    int mirror_count = 0;
    TestMirrorKState::Init();
    client->KStateResponseWait(1);
    mirror_count = TestKStateBase::fetched_count_;

    CreateMirrorEntry();
    TestMirrorKState::Init(mirror_count);
    client->KStateResponseWait(1);
    DeleteMirrorEntry();
}

TEST_F(KStateTest, RouteDumpTest) {
    if (!ksync_init_) {
        int rt_count = 0;
        TestRouteKState::Init(false);
        client->KStateResponseWait(1);
        rt_count = 0;

        CreatePorts(0, 0, rt_count);
        //Addition of 2 vm ports in a new VN (VRF) will result in the following routes
        // 2 new routes added during vrf addition (169.254.1.1, 169.254.169.254(meta-data service))
        // 2 routes corresponding to the IP address of VM ports
        // 1 route for l2 of vm port
        // 2 routes corresponding to 2 vmports in default-vrf using meta-data ip address of VM ports.
        TestRouteKState::Init(true, rt_count + (MAX_TEST_FD * 2) + 3);
        client->KStateResponseWait(1);
        DeletePorts();
    }
}

TEST_F(KStateTest, FlowDumpTest) {
    TestFlowKState::Init(true, -1, 0);
    client->KStateResponseWait(1);

    CreatePortsWithPolicy();

    VmPortInterface *test0, *test1;
    test0 = VmPortInterfaceGet(input[0].intf_id);
    assert(test0);
    test1 = VmPortInterfaceGet(input[1].intf_id);
    assert(test1);

    int hash_id = 1;
    //Flow creation using IP packet
    TxIpPacketUtil(test0->GetInterfaceId(), vm1_ip, vm2_ip, 0, hash_id);
    client->WaitForIdle(2);
    EXPECT_TRUE(FlowGet("vrf3", vm1_ip, vm2_ip, 0, 0, 0, false, 
                        "vn3", "vn3", hash_id++));

    //Create flow in reverse direction
    TxIpPacketUtil(test1->GetInterfaceId(), vm2_ip, vm1_ip, 0, hash_id);
    client->WaitForIdle(2);
    EXPECT_TRUE(FlowGet("vrf3", vm2_ip, vm1_ip, 0, 0, 0, true, 
                        "vn3", "vn3", hash_id++));

    //Flow creation using TCP packet
    TxTcpPacketUtil(test0->GetInterfaceId(), vm1_ip, vm2_ip, 1000, 200, 
                    hash_id);
    client->WaitForIdle(2);
    EXPECT_TRUE(FlowGet("vrf3", vm1_ip, vm2_ip, 6, 1000, 200, false,
                        "vn3", "vn3", hash_id++));

    //Create flow in reverse direction and make sure it is linked to previous flow
    TxTcpPacketUtil(test1->GetInterfaceId(), vm2_ip, vm1_ip, 200, 1000, 
                    hash_id);
    client->WaitForIdle(2);
    EXPECT_TRUE(FlowGet("vrf3", vm2_ip, vm1_ip, 6, 200, 1000, true, 
                        "vn3", "vn3", hash_id++));
    EXPECT_EQ(4U, FlowTable::GetFlowTableObject()->Size());

    TestFlowKState::Init(true, -1, 6);
    client->KStateResponseWait(1);

    //cleanup
    client->EnqueueFlowFlush();
    client->WaitForIdle(2);
    WAIT_FOR(1000, 1000, (0 == FlowTable::GetFlowTableObject()->Size()));
    DeletePortsWithPolicy();
}

int main(int argc, char *argv[]) {
    int ret;
    GETUSERARGS();
    
    /* Supported only with non-ksync mode for now */
    ksync_init = false;

    client = TestInit(init_file, ksync_init, true, false);
    KStateTest::TestSetup(ksync_init);
    KState::SetMaxResponseCount(2);

    ::testing::InitGoogleTest(&argc, argv);
    ret = RUN_ALL_TESTS();
    KStateTest::TestTearDown();
    Agent::GetInstance()->GetEventManager()->Shutdown();
    AsioStop();
    return ret;
}
