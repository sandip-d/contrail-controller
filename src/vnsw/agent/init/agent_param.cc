/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

/*
 * Agent parameters are derived from 3 entities in increasing priority,
 * - System information
 * - Configuration file
 * - Parameters
 */

#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/stat.h>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/foreach.hpp>
#include <boost/program_options.hpp>

#include <base/logging.h>
#include <base/misc_utils.h>
#include <sandesh/sandesh_types.h>
#include <sandesh/sandesh.h>
#include <sandesh/sandesh_trace.h>

#include <cmn/agent_cmn.h>
#include <init/agent_param.h>
#include <vgw/cfg_vgw.h>

#include <uve/agent_stats.h>
#include <uve/flow_stats.h>

using namespace std;
using namespace boost::property_tree;
using boost::optional;
namespace opt = boost::program_options;

static bool GetIpAddress(const string &str, Ip4Address *addr) {
    boost::system::error_code ec;
    Ip4Address tmp = Ip4Address::from_string(str, ec);
    if (ec.value() != 0) {
        return false;
    }
    *addr = tmp;
    return true;
}

static void ParseVHost(const ptree &tree, const string &config_file,
                       AgentParam::PortInfo *port) {
    try {
        boost::system::error_code ec;
        optional<string> opt_str;

        if (opt_str = tree.get_optional<string>("config.agent.vhost.name")) {
            port->name_ = opt_str.get();
        } 

        if (opt_str = tree.get_optional<string>("config.agent.vhost.ip-address")) {
            ec = Ip4PrefixParse(opt_str.get(), &port->addr_, &port->plen_);
            if (ec != 0 || port->plen_ >= 32) {
                LOG(ERROR, "Error in config file <" << config_file 
                    << ">. Error parsing vhost ip-address from <" 
                    << opt_str.get() << ">");
                return;
            }
        }

        if (opt_str = tree.get_optional<string>("config.agent.vhost.gateway")) {
            if (GetIpAddress(opt_str.get(), &port->gw_) == false) {
                LOG(ERROR, "Error in config file <" << config_file 
                    << ">. Error parsing vhost gateway address from <" 
                    << opt_str.get() << ">");
            }
        }
    } catch (exception &e) {
        LOG(ERROR, "Error reading \"vhost\" node in config file <" 
            << config_file << ">. Error <" << e.what() << ">");
        return;
    }
}

static void ParseEthPort(const ptree &tree, const string &config_file,
                         string *port) {
    optional<string> opt_str;
    try {
        if (opt_str = tree.get_optional<string>("config.agent.eth-port.name")) {
            *port = opt_str.get();
        }
    } catch (exception &e) {
        LOG(ERROR, "Error reading \"eth-port\" node in config file <"
            << config_file << ">. Error <" << e.what() << ">");
    }
    return;
}

static void ParseXmppServer(ptree &tree, const string &config_file,
                            Ip4Address *server1, Ip4Address *server2) {
    BOOST_FOREACH(ptree::value_type &v,
                  tree.get_child("config.agent")) {
        optional<string> opt_str;
        try {
            if (v.first != "xmpp-server") {
                continue;
            }

            if (opt_str = v.second.get<string>("ip-address")) {
                Ip4Address addr;
                if (GetIpAddress(opt_str.get(), &addr) == false) {
                    LOG(ERROR, "Error in config file <" << config_file 
                        << ">. Error parsing xmpp server address from <" 
                        << opt_str.get() << ">");
                    continue;
                }
                if (server1->to_ulong() == 0) {
                    *server1 = addr;
                } else if (server2->to_ulong() == 0) {
                    *server2 = addr;
                }
            }
        } catch (exception &e) {
            LOG(ERROR, "Error reading \"xmpp-server\" node in config "
                "file <" << config_file << ">. Error <" << e.what() << ">");
            continue;
        }

    }
}

static void ParseDnsServer(ptree &tree, const string &config_file,
                           Ip4Address *server1, Ip4Address *server2) {
    BOOST_FOREACH(ptree::value_type &v,
                  tree.get_child("config.agent")) {
        optional<string> opt_str;
        try {
            if (v.first != "dns-server") {
                continue;
            }

            if (opt_str = v.second.get<string>("ip-address")) {
                Ip4Address addr;
                if (GetIpAddress(opt_str.get(), &addr) == false) {
                    LOG(ERROR, "Error in config file <" << config_file 
                        << ">. Error parsing xmpp server address from <" 
                        << opt_str.get() << ">");
                    continue;
                }
                if (server1->to_ulong() == 0) {
                    *server1 = addr;
                } else if (server2->to_ulong() == 0) {
                    *server2 = addr;
                }
            }
        } catch (exception &e) {
            LOG(ERROR, "Error reading \"dns-server\" node in config "
                "file <" << config_file << ">. Error <" << e.what() << ">");
            continue;
        }

    }
}

static void ParseDiscoveryServer(const ptree &node, const string &config_file,
                                 Ip4Address *server, int *instances) {
    try {
        optional<string> opt_str;
        if (opt_str = node.get_optional<string>
            ("config.agent.discovery-server.ip-address")) {
            if (GetIpAddress(opt_str.get(), server) == false) {
                LOG(ERROR, "Error in config file <" << config_file 
                    << ">. Error parsing discovery server address from <" 
                    << opt_str.get() << ">");
            }
        }

        optional<int> opt_int;
        if (opt_int = node.get_optional<int>
            ("config.agent.discovery-server.control-instances")) {
            *instances = opt_int.get();
        }
    } catch (exception &e) {
        LOG(ERROR, "Error reading \"discovery-server\" node in config "
            "file <" << config_file << ">. Error <" << e.what() << ">");
        return;
    }
}

static void ParseControl(const ptree &node, const string &config_file,
                         Ip4Address *server) {
    try {
        optional<string> opt_str;
        if (opt_str = node.get_optional<string>
            ("config.agent.control.ip-address")) {
            if (GetIpAddress(opt_str.get(), server) == false) {
                LOG(ERROR, "Error in config file <" << config_file 
                    << ">. Error parsing vhost gateway address from <" 
                    << opt_str.get() << ">");
            }
        }
    } catch (exception &e) {
        LOG(ERROR, "Error reading \"control stanza\" in config "
            "file <" << config_file << ">. Error <" << e.what() << ">");
        return;
    }
}

static void ParseHypervisor(const ptree &node, const string &config_file, 
                            AgentParam::Mode *mode,
                            AgentParam::PortInfo *port) {
    try {
        optional<string> opt_str;
        if (opt_str = node.get_optional<string>
            ("config.agent.hypervisor.<xmlattr>.mode")) {
            // Initialize mode to KVM. Will be overwritten for XEN later
            *mode = AgentParam::MODE_KVM;

            if (opt_str.get() == "xen") {
                *mode = AgentParam::MODE_XEN;
                if (opt_str = node.get_optional<string>
                    ("config.agent.hypervisor.xen-ll-port")) {
                    port->name_ = opt_str.get();
                }

                boost::system::error_code ec;
                if (opt_str = node.get_optional<string>
                    ("config.agent.hypervisor.xen-ll-ip-address")) {
                    ec = Ip4PrefixParse(opt_str.get(), &port->addr_,
                                        &port->plen_);
                    if (ec != 0 || port->plen_ >= 32) {
                        LOG(ERROR, "Error in config file <" << config_file 
                            << ">. Error parsing vhost ip-address from <" 
                            << opt_str.get() << ">");
                        return;
                    }
                }
            } else {
                *mode = AgentParam::MODE_KVM;
            }
        }
    } catch (exception &e) {
        LOG(ERROR, "Error reading \"hypervisor\" node in config file <"
            << config_file << ">. Error <" << e.what() << ">");
    }
}

static void ParseTunnelType(const ptree &node, const string &config_file, 
                            string *str) {
    try {
        optional<string> opt_str;
        if (opt_str = node.get_optional<string> ("config.agent.tunnel-type")) {
            *str = opt_str.get();
        }
    } catch (exception &e) {
        LOG(ERROR, "Error reading \"hypervisor\" node in config file <"
            << config_file << ">. Error <" << e.what() << ">");
    }

    if ((*str != "MPLSoUDP") && (*str != "VXLAN"))
        *str = "MPLSoGRE";
}

static void ParseMetadataProxy(const ptree &node, const string &config_file, 
                               string *secret) {
    try {
        optional<string> opt_str;
        if (opt_str = node.get_optional<string>
                      ("config.agent.metadata-proxy.shared-secret")) {
            *secret = opt_str.get();
        }
    } catch (exception &e) {
        LOG(ERROR, "Error reading \"metadata-proxy\" node in config file <"
            << config_file << ">. Error <" << e.what() << ">");
    }
}

// Initialize hypervisor mode based on system information
// If "/proc/xen" exists it means we are running in Xen dom0
void AgentParam::InitFromSystem() {
    boost::system::error_code error;
    host_name_ = boost::asio::ip::host_name(error);

    struct stat fstat;
    if (stat("/proc/xen", &fstat) == 0) {
        mode_ = MODE_XEN;
        LOG(INFO, "Found file /proc/xen. Initializing mode to XEN");
    }
    xen_ll_.addr_ = Ip4Address::from_string("169.254.0.1");
    xen_ll_.plen_ = 16;

    return;
}

// Update agent parameters from config file
void AgentParam::InitFromConfig() {
    // Read the file
    ifstream ifs(config_file_.c_str());
    string str((istreambuf_iterator<char>(ifs) ),
               (istreambuf_iterator<char>() ));
    istringstream sstream(str);

    // Read and parse XML
    ptree tree;
    try {
        read_xml(sstream, tree, xml_parser::trim_whitespace);
    } catch (exception &e) {
        LOG(ERROR, "Error reading config file <" << config_file_ 
            << ">. XML format error??? <" << e.what() << ">");
        return;
    } 

    // Look for config.agent node
    ptree agent;
    try {
        agent = tree.get_child("config.agent");
    } catch (exception &e) {
        LOG(ERROR, config_file_ << " : Error reading \"agent\" node in config "
            "file. Error <" << e.what() << ">");
        return;
    }

    ParseVHost(tree, config_file_, &vhost_);
    ParseEthPort(tree, config_file_, &eth_port_);
    ParseXmppServer(tree, config_file_, &xmpp_server_1_, &xmpp_server_2_);
    ParseDnsServer(tree, config_file_, &dns_server_1_, &dns_server_2_);
    ParseDiscoveryServer(tree, config_file_, &dss_server_,
                         &xmpp_instance_count_);
    ParseControl(tree, config_file_, &mgmt_ip_);
    ParseHypervisor(tree, config_file_, &mode_, &xen_ll_);
    ParseTunnelType(tree, config_file_, &tunnel_type_);
    ParseMetadataProxy(tree, config_file_, &metadata_shared_secret_);
    LOG(DEBUG, "Config file <" << config_file_ << "> read successfully.");
    return;
}

void AgentParam::InitFromArguments
    (const boost::program_options::variables_map &var_map) {

    if (var_map.count("log-local")) {
        log_local_ = true;
    }

    if (var_map.count("log-level")) {
        log_level_ = var_map["log-level"].as<string>();
    }

    if (var_map.count("log-category")) {
        log_category_ = var_map["log-category"].as<string>();
    }

    if (var_map.count("collector")) {
        Ip4Address addr;
        if (GetIpAddress(var_map["collector"].as<string>(), &addr)) {
            collector_ = addr;
        }
    }

    if (var_map.count("collector-port")) {
        collector_port_ = var_map["collector-port"].as<int>();
    }

    if (var_map.count("http-server-port")) {
        http_server_port_ = var_map["http-server-port"].as<int>();
    }

    if (var_map.count("host-name")) {
        host_name_ = var_map["host-name"].as<string>().c_str();
    }

    if (var_map.count("log-file")) {
        log_file_ = var_map["log-file"].as<string>();
    }

    if (var_map.count("hypervisor")) {
        if (var_map["hypervisor"].as<string>() == "xen") {
            mode_ = AgentParam::MODE_XEN;
        } else {
            mode_ = AgentParam::MODE_KVM;
        }
    }

    if (var_map.count("xen-ll-port")) {
        xen_ll_.name_ = var_map["xen-ll-port"].as<string>();
    }

    if (var_map.count("xen-ll-ip-address")) {
        Ip4Address addr;
        if (!GetIpAddress(var_map["xen-ll-ip-address"].as<string>(), &addr)) {
            LOG(ERROR, "Error parsing xen-ll-ip-address");
            exit(EINVAL);
        }
        xen_ll_.addr_ = addr;
    }

    if (var_map.count("xen-ll-prefix-len")) {
        xen_ll_.plen_ = var_map["xen-ll-prefix-len"].as<int>();
        if (xen_ll_.plen_ <= 0 || xen_ll_.plen_ >= 32) {
            LOG(ERROR, "Error parsing argument for xen-ll-prefix-len");
            exit(EINVAL);
        }
    }

    return;
}

void AgentParam::Validate() {
    // Validate vhost_name
    if (vhost_.name_ == "") {
        LOG(ERROR, "Configuration error. vhost interface name not specified");
        exit(EINVAL);
    }

    // Validate ethernet port
    if (eth_port_ == "") {
        LOG(ERROR, "Configuration error. eth_port not specified");
        exit(EINVAL);
    }

    // Set the prefix address for VHOST and XENLL interfaces
    uint32_t mask = vhost_.plen_ ? (0xFFFFFFFF << (32 - vhost_.plen_)) : 0;
    vhost_.prefix_ = Ip4Address(vhost_.addr_.to_ulong() & mask);

    mask = xen_ll_.plen_ ? (0xFFFFFFFF << (32 - xen_ll_.plen_)) : 0;
    xen_ll_.prefix_ = Ip4Address(xen_ll_.addr_.to_ulong() & mask);
}

void AgentParam::Init(const string &config_file, const string &program_name,
                      const boost::program_options::variables_map &var_map) {
    config_file_ = config_file;
    program_name_ = program_name;

    InitFromSystem();
    InitFromConfig();
    InitFromArguments(var_map);
    Validate();
    vgw_config_->Init(config_file.c_str());
    LogConfig();
}

void AgentParam::LogConfig() const {
    LOG(DEBUG, "vhost interface name        : " << vhost_.name_);
    LOG(DEBUG, "vhost IP Address            : " << vhost_.addr_.to_string() 
        << "/" << vhost_.plen_);
    LOG(DEBUG, "vhost gateway               : " << vhost_.gw_.to_string());
    LOG(DEBUG, "Ethernet port               : " << eth_port_);
    LOG(DEBUG, "XMPP Server-1               : " << xmpp_server_1_);
    LOG(DEBUG, "XMPP Server-2               : " << xmpp_server_2_);
    LOG(DEBUG, "DNS Server-1                : " << dns_server_1_);
    LOG(DEBUG, "DNS Server-2                : " << dns_server_2_);
    LOG(DEBUG, "Discovery Server            : " << dss_server_);
    LOG(DEBUG, "Controller Instances        : " << xmpp_instance_count_);
    LOG(DEBUG, "Tunnel-Type                 : " << tunnel_type_);
    LOG(DEBUG, "Metadata-Proxy Shared Secret: " << metadata_shared_secret_);
    if (mode_ != MODE_XEN) {
    LOG(DEBUG, "Hypervisor mode             : kvm");
        return;
    }
    LOG(DEBUG, "Hypervisor mode             : xen");
    LOG(DEBUG, "XEN Link Local port         : " << xen_ll_.name_);
    LOG(DEBUG, "XEN Link Local IP Address   : " << xen_ll_.addr_.to_string()
        << "/" << xen_ll_.plen_);
}

AgentParam::AgentParam() :
        vhost_(), eth_port_(), xmpp_instance_count_(), xmpp_server_1_(),
        xmpp_server_2_(), dns_server_1_(), dns_server_2_(), dss_server_(),
        mgmt_ip_(), mode_(MODE_KVM), xen_ll_(), tunnel_type_(),
        metadata_shared_secret_(), config_file_(),
        program_name_(), log_file_(), log_local_(false), log_level_(),
        log_category_(), collector_(), collector_port_(), http_server_port_(),
        host_name_(),
        agent_stats_interval_(AgentStatsCollector::AgentStatsInterval), 
        flow_stats_interval_(FlowStatsCollector::FlowStatsInterval) {
    vgw_config_ = std::auto_ptr<VirtualGatewayConfig>
        (new VirtualGatewayConfig());
}
