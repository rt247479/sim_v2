/* Tiered Edge Computing Simulation with Smart Caching
 * Four Models: Traditional CDN, MEC-CDN, Fog-CDN, Hybrid MEC-Fog-CDN
 * Usage: ./ns3 run "scratch/tiered-edge --model=2 --tier1Cities=2 --tier2Cities=3"
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/wifi-module.h"
#include <unordered_map>
#include <random>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("TieredEdge");

// Configuration
uint32_t simulationModel = 2;  // 0=CDN, 1=MEC-CDN, 2=Fog-CDN, 3=Hybrid
uint32_t tier1Cities = 2;      // High-demand cities (MEC deployment)
uint32_t tier2Cities = 3;      // Low-demand cities (Fog deployment)
uint32_t usersPerCity = 50;    // Users per city
double simTime = 60.0;
uint32_t cacheSize = 50;       // Cache entries per edge node
uint32_t mecCapacity = 10;     // MEC computational capacity
uint32_t fogCapacity = 5;      // Fog computational capacity

// Global metrics
uint64_t g_totalRequests = 0;
uint64_t g_cacheHits = 0;
uint64_t g_peerHits = 0;
uint64_t g_cdnHits = 0;
uint64_t g_edgeComputes = 0;

// Content patterns for realistic workload
std::vector<std::string> videoContent = {"movie1", "series1", "live_stream1", "sports1"};
std::vector<std::string> socialContent = {"feed1", "story1", "post1", "reel1"};
std::vector<std::string> gamingContent = {"game_update1", "match_data1", "leaderboard1"};
std::vector<std::string> webContent = {"news1", "shopping1", "search1", "weather1"};

class SmartEdgeNode : public Application {
public:
    static TypeId GetTypeId(void) {
        static TypeId tid = TypeId("SmartEdgeNode")
            .SetParent<Application>()
            .AddConstructor<SmartEdgeNode>();
        return tid;
    }

    SmartEdgeNode() : m_socket(nullptr), m_capacity(0), m_currentLoad(0) {}
    
    void Setup(Address upstream, std::string nodeType, uint32_t capacity, 
               uint32_t cacheSz, std::vector<Address> peers = {});
    void SetCityTier(uint32_t tier) { m_cityTier = tier; }
    void InitializePopularContent();

private:
    virtual void StartApplication(void);
    virtual void StopApplication(void);
    void HandleRead(Ptr<Socket> socket);
    void ProcessRequest(const std::string& content, Address from);
    void ForwardToUpstream(const std::string& content, Address from);
    void ComputeAndCache(const std::string& content, Address from);
    void SendResponse(const std::string& content, Address dest);
    void QueryPeers(const std::string& content, Address originalRequester);
    void HandlePeerResponse(const std::string& response, Address from);
    
    std::string GenerateRealisticContent();
    ContentCategory ClassifyContent(const std::string& content);
    double CalculatePopularity(const std::string& content);
    void UpdateGlobalPopularity();
    
    Ptr<Socket> m_socket;
    Address m_upstream;
    std::string m_nodeType;  // "MEC", "Fog", "CDN"
    uint32_t m_capacity;
    uint32_t m_currentLoad;
    uint32_t m_cityTier;     // 1 or 2
    std::vector<Address> m_peers;
    
    // Smart caching components
    std::unique_ptr<PopularityLRUCache<std::string, bool>> m_cache;
    std::unique_ptr<PopularityTracker> m_tracker;
    std::unique_ptr<P2PCacheManager> m_p2pManager;
    
    // Request tracking
    struct PendingRequest {
        std::string content;
        Address requester;
        uint32_t pendingPeers;
        bool foundInPeer;
        Time requestTime;
    };
    std::unordered_map<std::string, PendingRequest> m_pendingRequests;
    
    // Workload characteristics based on city tier
    std::mt19937 m_rng;
    std::uniform_real_distribution<> m_dist;
};

void SmartEdgeNode::Setup(Address upstream, std::string nodeType, uint32_t capacity,
                         uint32_t cacheSz, std::vector<Address> peers) {
    m_upstream = upstream;
    m_nodeType = nodeType;
    m_capacity = capacity;
    m_peers = peers;
    
    // Initialize smart caching components
    m_tracker = std::make_unique<PopularityTracker>();
    m_cache = std::make_unique<PopularityLRUCache<std::string, bool>>(cacheSz, m_tracker.get());
    m_p2pManager = std::make_unique<P2PCacheManager>(m_tracker.get());
    
    for (const auto& peer : peers) {
        m_p2pManager->AddPeer(peer);
    }
    
    m_rng.seed(std::random_device{}());
    InitializePopularContent();
}

void SmartEdgeNode::InitializePopularContent() {
    // Pre-populate cache with popular content based on city tier
    std::vector<std::string> popularContent;
    
    if (m_cityTier == 1) {  // Tier-1: High-demand content
        popularContent.insert(popularContent.end(), videoContent.begin(), videoContent.end());
        popularContent.insert(popularContent.end(), gamingContent.begin(), gamingContent.end());
    } else {  // Tier-2: Basic content
        popularContent.insert(popularContent.end(), webContent.begin(), webContent.end());
        popularContent.insert(popularContent.end(), socialContent.begin(), socialContent.end());
    }
    
    for (const auto& content : popularContent) {
        ContentCategory category = ClassifyContent(content);
        m_tracker->RegisterContent(content, category, 1400, 0.5, {content});
        m_cache->Insert(content, true);
    }
}

void SmartEdgeNode::StartApplication(void) {
    m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), 50000));
    m_socket->SetRecvCallback(MakeCallback(&SmartEdgeNode::HandleRead, this));
}

void SmartEdgeNode::StopApplication(void) {
    if (m_socket) m_socket->Close();
}

void SmartEdgeNode::HandleRead(Ptr<Socket> socket) {
    Address from;
    Ptr<Packet> pkt = socket->RecvFrom(from);
    uint8_t buf[2048];
    uint32_t len = pkt->CopyData(buf, sizeof(buf));
    std::string content(reinterpret_cast<char*>(buf), len);
    
    g_totalRequests++;
    
    // Handle peer responses
    if (content.rfind("PEER_HIT:", 0) == 0) {
        HandlePeerResponse(content, from);
        return;
    }
    if (content.rfind("PEER_MISS:", 0) == 0) {
        HandlePeerResponse(content, from);
        return;
    }
    
    // Handle peer queries
    if (content.rfind("PEER_QUERY:", 0) == 0) {
        std::string requestedContent = content.substr(11);
        std::string response = m_cache->Contains(requestedContent) ? 
                              "PEER_HIT:" + requestedContent : 
                              "PEER_MISS:" + requestedContent;
        SendResponse(response, from);
        return;
    }
    
    ProcessRequest(content, from);
}

void SmartEdgeNode::ProcessRequest(const std::string& content, Address from) {
    // Update popularity tracking
    m_tracker->RecordAccess(content);
    
    // Check local cache first
    if (m_cache->Contains(content)) {
        g_cacheHits++;
        SendResponse(content, from);
        return;
    }
    
    // If edge node (MEC/Fog), try peer sharing
    if ((m_nodeType == "MEC" || m_nodeType == "Fog") && !m_peers.empty()) {
        QueryPeers(content, from);
        return;
    }
    
    // If overloaded or no peers, forward upstream
    if (m_currentLoad >= m_capacity) {
        ForwardToUpstream(content, from);
        return;
    }
    
    // Process locally
    ComputeAndCache(content, from);
}

void SmartEdgeNode::QueryPeers(const std::string& content, Address originalRequester) {
    PendingRequest req;
    req.content = content;
    req.requester = originalRequester;
    req.pendingPeers = m_peers.size();
    req.foundInPeer = false;
    req.requestTime = Simulator::Now();
    
    m_pendingRequests[content] = req;
    
    // Query all peers
    for (const auto& peer : m_peers) {
        std::string query = "PEER_QUERY:" + content;
        Ptr<Socket> peerSocket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        peerSocket->Connect(peer);
        peerSocket->Send(Create<Packet>((uint8_t*)query.c_str(), query.size()));
        peerSocket->Close();
    }
}

void SmartEdgeNode::HandlePeerResponse(const std::string& response, Address from) {
    bool isHit = (response.rfind("PEER_HIT:", 0) == 0);
    std::string content = response.substr(isHit ? 9 : 10);
    
    auto it = m_pendingRequests.find(content);
    if (it == m_pendingRequests.end()) return;
    
    if (isHit && !it->second.foundInPeer) {
        g_peerHits++;
        it->second.foundInPeer = true;
        SendResponse(content, it->second.requester);
        m_cache->Insert(content, true);  // Cache for future use
    }
    
    it->second.pendingPeers--;
    if (it->second.pendingPeers == 0) {
        if (!it->second.foundInPeer) {
            // No peer had it, process locally or forward upstream
            if (m_currentLoad < m_capacity) {
                ComputeAndCache(content, it->second.requester);
            } else {
                ForwardToUpstream(content, it->second.requester);
            }
        }
        m_pendingRequests.erase(it);
    }
}

void SmartEdgeNode::ForwardToUpstream(const std::string& content, Address from) {
    if (m_nodeType == "CDN") {
        g_cdnHits++;
        // CDN always has content
        Simulator::Schedule(MilliSeconds(50), &SmartEdgeNode::SendResponse, this, content, from);
        return;
    }
    
    Ptr<Socket> upSocket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    upSocket->Connect(m_upstream);
    upSocket->Send(Create<Packet>((uint8_t*)content.c_str(), content.size()));
    upSocket->Close();
}

void SmartEdgeNode::ComputeAndCache(const std::string& content, Address from) {
    m_currentLoad++;
    g_edgeComputes++;
    
    // Simulate processing time based on content complexity
    Time processTime = MilliSeconds(10);  // Base processing time
    ContentCategory category = ClassifyContent(content);
    
    switch (category) {
        case VIDEO_STREAMING:
            processTime = MilliSeconds(50);
            break;
        case GAMING:
            processTime = MilliSeconds(20);
            break;
        case AUGMENTED_REALITY:
            processTime = MilliSeconds(30);
            break;
        default:
            processTime = MilliSeconds(10);
    }
    
    // If tier-1 city with MEC, faster processing
    if (m_cityTier == 1 && m_nodeType == "MEC") {
        processTime = processTime * 0.5;  // MEC is 2x faster
    }
    
    Simulator::Schedule(processTime, [this, content, from]() {
        m_cache->Insert(content, true);
        SendResponse(content, from);
        m_currentLoad--;
        
        // Share popularity update with peers
        double popularity = CalculatePopularity(content);
        m_p2pManager->SharePopularityUpdate(content, popularity);
    });
}

void SmartEdgeNode::SendResponse(const std::string& content, Address dest) {
    Ptr<Packet> pkt = Create<Packet>((uint8_t*)content.c_str(), content.size());
    m_socket->SendTo(pkt, 0, dest);
}

ContentCategory SmartEdgeNode::ClassifyContent(const std::string& content) {
    if (content.find("movie") != std::string::npos || 
        content.find("stream") != std::string::npos) return VIDEO_STREAMING;
    if (content.find("game") != std::string::npos) return GAMING;
    if (content.find("feed") != std::string::npos || 
        content.find("post") != std::string::npos) return SOCIAL_MEDIA;
    if (content.find("ar_") != std::string::npos) return AUGMENTED_REALITY;
    return WEB_BROWSING;
}

double SmartEdgeNode::CalculatePopularity(const std::string& content) {
    auto metadata = m_tracker->GetContentInfo(content);
    return metadata.popularityScore;
}

void CreateTieredTopology() {
    // Create nodes based on selected model
    NodeContainer cdnNodes, mecNodes, fogNodes, userNodes;
    
    cdnNodes.Create(1);  // Single CDN
    
    if (simulationModel == 1 || simulationModel == 3) {
        mecNodes.Create(tier1Cities);  // MEC nodes for tier-1 cities
    }
    if (simulationModel == 2 || simulationModel == 3) {
        fogNodes.Create(tier2Cities);  // Fog nodes for tier-2 cities
    }
    
    uint32_t totalCities = (simulationModel == 3) ? tier1Cities + tier2Cities : 
                          (simulationModel == 1) ? tier1Cities : tier2Cities;
    userNodes.Create(totalCities * usersPerCity);
    
    // Install Internet stack
    InternetStackHelper stack;
    stack.InstallAll();
    
    // Create network topology
    PointToPointHelper p2pBackbone, p2pEdge;
    p2pBackbone.SetDeviceAttribute("DataRate", StringValue("10Gbps"));
    p2pBackbone.SetChannelAttribute("Delay", StringValue("20ms"));
    
    p2pEdge.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2pEdge.SetChannelAttribute("Delay", StringValue("5ms"));
    
    // Connect CDN to edge nodes
    NetDeviceContainer cdnConnections;
    if (simulationModel == 1 || simulationModel == 3) {
        for (uint32_t i = 0; i < mecNodes.GetN(); ++i) {
            cdnConnections.Add(p2pBackbone.Install(cdnNodes.Get(0), mecNodes.Get(i)));
        }
    }
    if (simulationModel == 2 || simulationModel == 3) {
        for (uint32_t i = 0; i < fogNodes.GetN(); ++i) {
            cdnConnections.Add(p2pBackbone.Install(cdnNodes.Get(0), fogNodes.Get(i)));
        }
    }
    
    // Connect users to edge nodes or directly to CDN
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211n);
    WifiMacHelper mac;
    YansWifiPhyHelper phy;
    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
    phy.SetChannel(channel.Create());
    
    NetDeviceContainer userConnections;
    uint32_t userIndex = 0;
    
    // Connect users based on simulation model
    for (uint32_t city = 0; city < totalCities; ++city) {
        NodeContainer cityUsers;
        for (uint32_t u = 0; u < usersPerCity; ++u) {
            cityUsers.Add(userNodes.Get(userIndex++));
        }
        
        if (simulationModel == 0) {
            // Traditional CDN: direct connection
            for (uint32_t u = 0; u < cityUsers.GetN(); ++u) {
                userConnections.Add(p2pEdge.Install(cityUsers.Get(u), cdnNodes.Get(0)));
            }
        } else {
            // Connect to appropriate edge node
            Ptr<Node> edgeNode;
            if (simulationModel == 1) {
                edgeNode = mecNodes.Get(city % mecNodes.GetN());
            } else if (simulationModel == 2) {
                edgeNode = fogNodes.Get(city % fogNodes.GetN());
            } else {  // Hybrid model
                if (city < tier1Cities) {
                    edgeNode = mecNodes.Get(city);
                } else {
                    edgeNode = fogNodes.Get(city - tier1Cities);
                }
            }
            
            // Create WiFi network for the city
            mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(Ssid("city-" + std::to_string(city))));
            NetDeviceContainer staDevices = wifi.Install(phy, mac, cityUsers);
            
            mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(Ssid("city-" + std::to_string(city))));
            NetDeviceContainer apDevice = wifi.Install(phy, mac, edgeNode);
            
            userConnections.Add(staDevices);
            userConnections.Add(apDevice);
        }
    }
    
    // Assign IP addresses
    Ipv4AddressHelper address;
    address.SetBase("10.1.0.0", "255.255.0.0");
    address.Assign(cdnConnections);
    
    address.SetBase("10.2.0.0", "255.255.0.0");
    address.Assign(userConnections);
    
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();
    
    // Install applications
    InstallApplications(cdnNodes, mecNodes, fogNodes, userNodes);
}

void InstallApplications(NodeContainer& cdnNodes, NodeContainer& mecNodes, 
                        NodeContainer& fogNodes, NodeContainer& userNodes) {
    
    Address cdnAddr = InetSocketAddress(
        cdnNodes.Get(0)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal(), 50000);
    
    // Install CDN application
    Ptr<SmartEdgeNode> cdnApp = CreateObject<SmartEdgeNode>();
    cdnApp->Setup(Address(), "CDN", UINT32_MAX, cacheSize * 10);  // Large CDN cache
    cdnNodes.Get(0)->AddApplication(cdnApp);
    
    // Install MEC applications
    std::vector<Address> mecPeers;
    for (uint32_t i = 0; i < mecNodes.GetN(); ++i) {
        mecPeers.push_back(InetSocketAddress(
            mecNodes.Get(i)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal(), 50000));
    }
    
    for (uint32_t i = 0; i < mecNodes.GetN(); ++i) {
        std::vector<Address> peers;
        for (uint32_t j = 0; j < mecNodes.GetN(); ++j) {
            if (i != j) peers.push_back(mecPeers[j]);
        }
        
        Ptr<SmartEdgeNode> mecApp = CreateObject<SmartEdgeNode>();
        mecApp->Setup(cdnAddr, "MEC", mecCapacity, cacheSize, peers);
        mecApp->SetCityTier(1);  // Tier-1 city
        mecNodes.Get(i)->AddApplication(mecApp);
    }
    
    // Install Fog applications
    std::vector<Address> fogPeers;
    for (uint32_t i = 0; i < fogNodes.GetN(); ++i) {
        fogPeers.push_back(InetSocketAddress(
            fogNodes.Get(i)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal(), 50000));
    }
    
    for (uint32_t i = 0; i < fogNodes.GetN(); ++i) {
        std::vector<Address> peers;
        for (uint32_t j = 0; j < fogNodes.GetN(); ++j) {
            if (i != j) peers.push_back(fogPeers[j]);
        }
        
        // In hybrid model, Fog can also peer with MEC
        if (simulationModel == 3) {
            peers.insert(peers.end(), mecPeers.begin(), mecPeers.end());
        }
        
        Ptr<SmartEdgeNode> fogApp = CreateObject<SmartEdgeNode>();
        fogApp->Setup(cdnAddr, "Fog", fogCapacity, cacheSize, peers);
        fogApp->SetCityTier(2);  // Tier-2 city
        fogNodes.Get(i)->AddApplication(fogApp);
    }
    
    // Install user applications with realistic traffic patterns
    InstallUserTraffic(userNodes, mecNodes, fogNodes, cdnNodes);
}

void InstallUserTraffic(NodeContainer& userNodes, NodeContainer& mecNodes, 
                       NodeContainer& fogNodes, NodeContainer& cdnNodes) {
    
    std::random_device rd;
    std::mt19937 gen(rd());
    
    // Content distribution based on real-world patterns
    std::vector<std::string> contentPool;
    
    // Video content (40% - most popular)
    for (int i = 0; i < 40; i++) {
        contentPool.push_back("video_" + std::to_string(i));
    }
    
    // Social media (25%)
    for (int i = 0; i < 25; i++) {
        contentPool.push_back("social_" + std::to_string(i));
    }
    
    // Gaming (20%)
    for (int i = 0; i < 20; i++) {
        contentPool.push_back("game_" + std::to_string(i));
    }
    
    // Web browsing (15%)
    for (int i = 0; i < 15; i++) {
        contentPool.push_back("web_" + std::to_string(i));
    }
    
    // Create Zipf distribution for content popularity
    std::discrete_distribution<> zipf_dist;
    std::vector<double> weights;
    for (size_t i = 1; i <= contentPool.size(); ++i) {
        weights.push_back(1.0 / std::pow(i, 1.2));  // Zipf parameter = 1.2
    }
    zipf_dist = std::discrete_distribution<>(weights.begin(), weights.end());
    
    // Install traffic generators for users
    for (uint32_t u = 0; u < userNodes.GetN(); ++u) {
        Ptr<Node> user = userNodes.Get(u);
        uint32_t cityId = u / usersPerCity;
        
        // Determine target server based on model
        Address targetAddr;
        switch (simulationModel) {
            case 0:  // Traditional CDN
                targetAddr = InetSocketAddress(
                    cdnNodes.Get(0)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal(), 50000);
                break;
            case 1:  // MEC-CDN
                targetAddr = InetSocketAddress(
                    mecNodes.Get(cityId % mecNodes.GetN())->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal(), 50000);
                break;
            case 2:  // Fog-CDN
                targetAddr = InetSocketAddress(
                    fogNodes.Get(cityId % fogNodes.GetN())->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal(), 50000);
                break;
            case 3:  // Hybrid
                if (cityId < tier1Cities) {
                    targetAddr = InetSocketAddress(
                        mecNodes.Get(cityId)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal(), 50000);
                } else {
                    targetAddr = InetSocketAddress(
                        fogNodes.Get(cityId - tier1Cities)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal(), 50000);
                }
                break;
        }
        
        // Create custom traffic generator
        Ptr<RealisticTrafficApp> trafficApp = CreateObject<RealisticTrafficApp>();
        trafficApp->Setup(targetAddr, contentPool, zipf_dist);
        user->AddApplication(trafficApp);
        trafficApp->SetStartTime(Seconds(1.0 + u * 0.01));  // Staggered start
    }
}

// Realistic Traffic Generator
class RealisticTrafficApp : public Application {
public:
    static TypeId GetTypeId(void) {
        static TypeId tid = TypeId("RealisticTrafficApp")
            .SetParent<Application>()
            .AddConstructor<RealisticTrafficApp>();
        return tid;
    }
    
    void Setup(Address target, const std::vector<std::string>& content, 
               std::discrete_distribution<>& dist) {
        m_target = target;
        m_contentPool = content;
        m_distribution = dist;
        m_rng.seed(std::random_device{}());
    }
    
private:
    virtual void StartApplication(void) {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        ScheduleNextRequest();
    }
    
    virtual void StopApplication(void) {
        if (m_socket) m_socket->Close();
        Simulator::Cancel(m_nextEvent);
    }
    
    void ScheduleNextRequest() {
        // Variable inter-request time based on user behavior
        std::exponential_distribution<> interArrival(2.0);  // 2 requests per second average
        Time nextTime = Seconds(interArrival(m_rng));
        
        m_nextEvent = Simulator::Schedule(nextTime, &RealisticTrafficApp::SendRequest, this);
    }
    
    void SendRequest() {
        // Select content based on popularity distribution
        uint32_t contentIndex = m_distribution(m_rng);
        std::string content = m_contentPool[contentIndex];
        
        // Add user context for personalization
        content += "_user" + std::to_string(GetNode()->GetId());
        
        Ptr<Packet> pkt = Create<Packet>((uint8_t*)content.c_str(), content.size());
        m_socket->Connect(m_target);
        m_socket->Send(pkt);
        
        ScheduleNextRequest();
    }
    
    Ptr<Socket> m_socket;
    Address m_target;
    std::vector<std::string> m_contentPool;
    std::discrete_distribution<> m_distribution;
    std::mt19937 m_rng;
    EventId m_nextEvent;
};

int main(int argc, char *argv[]) {
    CommandLine cmd;
    cmd.AddValue("model", "Simulation model: 0=CDN, 1=MEC-CDN, 2=Fog-CDN, 3=Hybrid", simulationModel);
    cmd.AddValue("tier1Cities", "Number of tier-1 cities (MEC)", tier1Cities);
    cmd.AddValue("tier2Cities", "Number of tier-2 cities (Fog)", tier2Cities);
    cmd.AddValue("usersPerCity", "Users per city", usersPerCity);
    cmd.AddValue("simTime", "Simulation time (s)", simTime);
    cmd.AddValue("cacheSize", "Cache size per edge node", cacheSize);
    cmd.AddValue("mecCapacity", "MEC computational capacity", mecCapacity);
    cmd.AddValue("fogCapacity", "Fog computational capacity", fogCapacity);
    cmd.Parse(argc, argv);
    
    std::string modelNames[] = {"Traditional CDN", "MEC-CDN", "Fog-CDN", "Hybrid MEC-Fog-CDN"};
    std::cout << "\n=== Running " << modelNames[simulationModel] << " Simulation ===\n";
    std::cout << "Tier-1 Cities (MEC): " << tier1Cities << "\n";
    std::cout << "Tier-2 Cities (Fog): " << tier2Cities << "\n";
    std::cout << "Users per City: " << usersPerCity << "\n";
    std::cout << "Cache Size: " << cacheSize << "\n\n";
    
    CreateTieredTopology();
    
    // Install flow monitor
    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();
    
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    
    // Calculate and display results
    monitor->CheckForLostPackets();
    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();
    
    double totalLatency = 0.0, totalThroughput = 0.0;
    uint64_t totalBytes = 0;
    uint32_t flowCount = 0;
    
    for (const auto& flow : stats) {
        if (flow.second.rxPackets > 0) {
            totalLatency += flow.second.delaySum.GetSeconds();
            totalThroughput += flow.second.rxBytes * 8.0 / simTime;  // bps
            totalBytes += flow.second.rxBytes;
            flowCount++;
        }
    }
    
    double avgLatency = flowCount > 0 ? totalLatency / flowCount : 0.0;
    double cacheHitRatio = g_totalRequests > 0 ? (double)g_cacheHits / g_totalRequests * 100.0 : 0.0;
    double peerHitRatio = g_totalRequests > 0 ? (double)g_peerHits / g_totalRequests * 100.0 : 0.0;
    double cdnOffloadRatio = g_totalRequests > 0 ? (double)(g_totalRequests - g_cdnHits) / g_totalRequests * 100.0 : 0.0;
    
    std::cout << "\n=== Simulation Results ===\n";
    std::cout << "Total Requests: " << g_totalRequests << "\n";
    std::cout << "Average Latency: " << avgLatency * 1000 << " ms\n";
    std::cout << "Total Throughput: " << totalThroughput / 1e6 << " Mbps\n";
    std::cout << "Local Cache Hit Ratio: " << cacheHitRatio << "%\n";
    std::cout << "Peer Cache Hit Ratio: " << peerHitRatio << "%\n";
    std::cout << "CDN Offload Ratio: " << cdnOffloadRatio << "%\n";
    std::cout << "Edge Computations: " << g_edgeComputes << "\n";
    std::cout << "CDN Requests: " << g_cdnHits << "\n";
    
    // Performance comparison
    std::cout << "\n=== Performance Insights ===\n";
    if (simulationModel == 0) {
        std::cout << "Baseline CDN performance established.\n";
    } else {
        std::cout << "Edge computing benefits:\n";
        std::cout << "- Reduced CDN load: " << cdnOffloadRatio << "%\n";
        std::cout << "- Local processing: " << g_edgeComputes << " tasks\n";
        if (peerHitRatio > 0) {
            std::cout << "- P2P cache sharing effectiveness: " << peerHitRatio << "%\n";
        }
    }
    
    Simulator::Destroy();
    return 0;
}
