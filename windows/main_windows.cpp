// Windows C++17 client. EAPOL uses Npcap; UDP uses Winsock.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include "../protocol.hpp"
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <iomanip>
#include <memory>

#ifdef _MSC_VER
#include <BaseTsd.h>
using ssize_t=SSIZE_T;
#endif

using namespace scut;
using Clock=std::chrono::steady_clock;
using Time=Clock::time_point;
static volatile std::sig_atomic_t stopping=0;
static HANDLE stop_event=nullptr;
static void signal_stop(int) { stopping=1; }
static bool stop_requested() {
    if(stop_event && WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0) stopping=1;
    return stopping!=0;
}
static void log(const std::string& message) { std::cout<<message<<std::endl; }
static void syscheck(bool ok,const std::string& what) {
    if(!ok) throw std::runtime_error(what+" (Winsock error "+std::to_string(WSAGetLastError())+")");
}
struct Fd {
    SOCKET n=INVALID_SOCKET;
    Fd()=default; explicit Fd(SOCKET value):n(value){}
    Fd(const Fd&)=delete; Fd& operator=(const Fd&)=delete;
    ~Fd(){ reset(); }
    void reset(SOCKET value=INVALID_SOCKET){ if(n!=INVALID_SOCKET) closesocket(n); n=value; }
};

struct Wsa {
    Wsa() { WSADATA data{}; require(WSAStartup(MAKEWORD(2,2),&data)==0,"WSAStartup failed"); }
    ~Wsa() { WSACleanup(); }
};

// Minimal Npcap ABI declarations. The DLL is loaded dynamically, so building
// does not require the Npcap SDK or import library. Npcap remains a runtime
// prerequisite and must be installed with WinPcap API compatibility enabled.
struct pcap;
using pcap_t=pcap;
struct pcap_pkthdr { timeval ts; unsigned caplen; unsigned len; };
class PcapApi {
    HMODULE packet_=nullptr,module_=nullptr;
    template<class T> T symbol(const char* name) {
        FARPROC raw=GetProcAddress(module_,name);
        if(!raw) throw std::runtime_error(std::string("Npcap is missing symbol ")+name);
        static_assert(sizeof(T)==sizeof(raw),"unexpected Windows function pointer size");
        T value{}; std::memcpy(&value,&raw,sizeof value);
        return value;
    }
public:
    using Open=pcap_t* (__cdecl*)(const char*,int,int,int,char*);
    using Close=void (__cdecl*)(pcap_t*);
    using Send=int (__cdecl*)(pcap_t*,const unsigned char*,int);
    using Next=int (__cdecl*)(pcap_t*,pcap_pkthdr**,const unsigned char**);
    using Error=char* (__cdecl*)(pcap_t*);
    Open open_live{}; Close close{}; Send sendpacket{}; Next next_ex{}; Error geterr{};
    PcapApi() {
        const wchar_t* packet_path=L"C:\\Windows\\System32\\Npcap\\Packet.dll";
        const wchar_t* wpcap_path=L"C:\\Windows\\System32\\Npcap\\wpcap.dll";
        packet_=LoadLibraryExW(packet_path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        module_=LoadLibraryExW(wpcap_path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module_) module_=LoadLibraryW(L"wpcap.dll");
        if(!module_) throw std::runtime_error("Npcap wpcap.dll not found; install Npcap first");
        open_live=symbol<Open>("pcap_open_live"); close=symbol<Close>("pcap_close");
        sendpacket=symbol<Send>("pcap_sendpacket"); next_ex=symbol<Next>("pcap_next_ex");
        geterr=symbol<Error>("pcap_geterr");
    }
    ~PcapApi() { if(module_) FreeLibrary(module_); if(packet_) FreeLibrary(packet_); }
    PcapApi(const PcapApi&)=delete; PcapApi& operator=(const PcapApi&)=delete;
};

static std::string utf8(const wchar_t* value) {
    if(!value) return {};
    const int count=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);
    if(count<=1) return {};
    std::string out(size_t(count),'\0');
    WideCharToMultiByte(CP_UTF8,0,value,-1,out.data(),count,nullptr,nullptr);
    out.pop_back();
    return out;
}
static std::string lower_ascii(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){ return char(std::tolower(c)); });
    return value;
}
static std::string trim(std::string s) {
    const auto first=s.find_first_not_of(" \r\n\t");
    if(first==std::string::npos) return {};
    return s.substr(first,s.find_last_not_of(" \r\n\t")-first+1);
}
static IP parse_ip(const std::string& s) {
    IP ip{}; require(inet_pton(AF_INET,s.c_str(),ip.data())==1,"invalid IPv4 address"); return ip;
}
static std::string ip_text(const IP& ip) {
    char s[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET,ip.data(),s,sizeof s); return s;
}
static Mac parse_mac(const std::string& s) {
    std::string compact;
    for(char c:s) if(c!=':' && c!='-') compact.push_back(c);
    require(compact.size()==12,"invalid MAC address");
    const Bytes bytes=unhex(compact); Mac mac{}; std::copy_n(bytes.begin(),6,mac.begin()); return mac;
}
static sockaddr_in address(const IP& ip,uint16_t port) {
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port); std::memcpy(&a.sin_addr,ip.data(),4); return a;
}
static int number(const std::string& s,int low,int high) {
    size_t end=0; int v=std::stoi(s,&end);
    require(end==s.size() && v>=low && v<=high,"numeric option out of range"); return v;
}
struct Config {
    std::string iface,user,password,host="s.scut.edu.cn",hostname;
    std::string stop_event_name;
    std::string profile="windows-31";
    std::optional<IP> ip,udp_ip,gateway;
    std::optional<Mac> mac;
    Bytes udp_trailer=unhex("2001025030007004aa0cb7dee93f3c65");
    std::vector<IP> dns{parse_ip("202.38.193.33"),parse_ip("222.201.130.30"),parse_ip("202.112.17.33"),parse_ip("222.201.130.33")};
    int retry=2,interval=5000,reconnect=15,eap_timeout=60,udp_timeout=12,heartbeat_interval=12,wake_hour=7,wake_minute=0,run_seconds=0,udp_local_port=0;
    bool once=false,check=false;
};
static void usage() {
    std::cout<<"drcom4scut-cpp (Windows, IPv4, Npcap)\n"
        "  --config FILE          key=value config (NOT YAML)\n"
        "  --interface NAME       physical Ethernet friendly name, GUID or ifIndex\n"
        "  --ip IPV4              IPv4 embedded in EAP authentication packets\n"
        "  --udp-ip IPV4          IPv4 embedded in DrCOM UDP packets (default: interface IPv4)\n"
        "  --gateway IPV4         gateway embedded in windows-31 MiscInfo\n"
        "  --mac MAC              client MAC in packets (default: interface MAC)\n"
        "  --username USER        account; password comes from config\n"
        "  --host HOST_OR_IPV4    default s.scut.edu.cn\n"
        "  --dns IPV4[,IPV4...]   campus DNS servers\n"
        "  --profile NAME         legacy-2a or windows-31\n"
        "  --udp-local-port PORT  local UDP port (0: scan from 36144)\n"
        "  --udp-trailer HEX      16-byte windows-31 trailer\n"
        "  --hostname NAME       name advertised to server\n"
        "  --time HH:MM          resume after prohibited period (default 07:00)\n"
        "  --retry N --interval MS --reconnect SEC\n"
        "  --eap-timeout SEC --udp-timeout SEC\n"
        "  --heartbeat-interval SEC  interval between established UDP heartbeats (default: 12)\n"
        "  --list-interfaces      list interfaces, no authentication\n"
        "  --check-interface      open raw socket then exit, no packets sent\n"
        "  --once                 exit instead of reconnecting after failure\n"
        "  --run-seconds SEC      bounded run (0 means unlimited)\n"
        "  --stop-event NAME      Windows named event used by the GUI for clean stop\n"
        "  --help\n";
}
static Config config(int argc,char** argv) {
    std::map<std::string,std::string> values; std::string path;
    for(int i=1;i<argc;++i) if(std::string(argv[i])=="--config") {
        require(i+1<argc,"--config needs a value"); path=argv[++i];
    }
    if(!path.empty()) {
        std::ifstream file(path); require(bool(file),"cannot open config"); std::string line;
        while(std::getline(file,line)) {
            line=trim(line); if(line.empty() || line[0]=='#') continue;
            auto p=line.find('='); require(p!=std::string::npos,"config must use key=value");
            auto key=trim(line.substr(0,p)); auto val=trim(line.substr(p+1));
            // Quotes preserve leading/trailing password spaces. No inline comments.
            if(val.size()>=2 && ((val.front()=='\"'&&val.back()=='\"')||(val.front()=='\''&&val.back()=='\'')))
                val=val.substr(1,val.size()-2);
            values[key]=val;
        }
    }
    Config c; char name[256]{}; if(gethostname(name,sizeof(name)-1)==0) c.hostname=name;
    for(int i=1;i<argc;++i) {
        std::string key=argv[i];
        if(key=="--once") { c.once=true; continue; }
        if(key=="--check-interface") { c.check=true; continue; }
        require(key.rfind("--",0)==0 && i+1<argc,"invalid option or missing value");
        const std::string value=argv[++i]; if(key=="--config") continue;
        require(key!="--password","put password in the config file, not command arguments");
        values[key.substr(2)]=value;
    }
    for(const auto& [k,v]:values) {
        if(k=="interface") c.iface=v;
        else if(k=="ip") c.ip=parse_ip(v);
        else if(k=="udp-ip") c.udp_ip=parse_ip(v);
        else if(k=="gateway") c.gateway=parse_ip(v);
        else if(k=="mac") c.mac=parse_mac(v);
        else if(k=="username") c.user=v;
        else if(k=="password") c.password=v;
        else if(k=="host") c.host=v;
        else if(k=="hostname") c.hostname=v;
        else if(k=="stop-event") c.stop_event_name=v;
        else if(k=="profile") c.profile=v;
        else if(k=="udp-local-port") c.udp_local_port=number(v,0,65535);
        else if(k=="udp-trailer") { c.udp_trailer=unhex(v); require(c.udp_trailer.size()==16,"udp-trailer must be 16 bytes"); }
        else if(k=="retry") c.retry=number(v,0,100);
        else if(k=="interval") c.interval=number(v,100,600000);
        else if(k=="reconnect") c.reconnect=number(v,1,86400);
        else if(k=="eap-timeout") c.eap_timeout=number(v,1,3600);
        else if(k=="udp-timeout") c.udp_timeout=number(v,1,3600);
        else if(k=="heartbeat-interval") c.heartbeat_interval=number(v,1,86400);
        else if(k=="run-seconds") c.run_seconds=number(v,0,86400);
        else if(k=="time") {
            auto p=v.find(':'); require(p!=std::string::npos,"time must be HH:MM");
            c.wake_hour=number(v.substr(0,p),0,23); c.wake_minute=number(v.substr(p+1),0,59);
        } else if(k=="dns") {
            c.dns.clear(); std::stringstream parts(v); std::string part;
            while(std::getline(parts,part,',')) c.dns.push_back(parse_ip(trim(part)));
            require(!c.dns.empty(),"DNS list is empty");
        } else throw std::runtime_error("unknown config key: "+k);
    }
    require(!c.iface.empty() && c.iface.size()<=512,"specify --interface NAME");
    require(c.check || (!c.user.empty() && !c.password.empty()),"username and password are required");
    require(c.check || c.ip.has_value(),"ip is required for authentication");
    require(c.profile=="legacy-2a" || c.profile=="windows-31","profile must be legacy-2a or windows-31");
    if(!c.check && c.profile=="windows-31") {
        require(c.gateway.has_value(),"gateway is required for windows-31 profile");
        require(c.dns.size()>=2,"windows-31 profile requires at least two DNS addresses");
    }
    require(c.user.size()<=25,"username must be at most 25 UTF-8 bytes (UDP protocol limit)");
    require(!c.host.empty() && c.host.size()<=253,"invalid host length"); return c;
}
struct Adapter {
    std::string adapter_name,friendly,description,pcap_name;
    ULONG index=0,type=0; IF_OPER_STATUS status=IfOperStatusUnknown;
    Mac mac{}; IP ip{}; bool has_ip=false;
};

static std::vector<Adapter> adapters() {
    ULONG size=16*1024;
    std::vector<unsigned char> storage(size);
    auto* first=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG result=GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_PREFIX,nullptr,first,&size);
    if(result==ERROR_BUFFER_OVERFLOW) {
        storage.resize(size); first=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result=GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_PREFIX,nullptr,first,&size);
    }
    if(result!=NO_ERROR) throw std::runtime_error("GetAdaptersAddresses failed ("+std::to_string(result)+")");
    std::vector<Adapter> out;
    for(auto* item=first;item;item=item->Next) {
        Adapter a; a.adapter_name=item->AdapterName?item->AdapterName:"";
        a.friendly=utf8(item->FriendlyName); a.description=utf8(item->Description);
        a.pcap_name="\\Device\\NPF_"+a.adapter_name;
        a.index=item->IfIndex; a.type=item->IfType; a.status=item->OperStatus;
        if(item->PhysicalAddressLength==6) std::copy_n(item->PhysicalAddress,6,a.mac.begin());
        for(auto* address=item->FirstUnicastAddress;address;address=address->Next) {
            if(address->Address.lpSockaddr && address->Address.lpSockaddr->sa_family==AF_INET) {
                const auto* v4=reinterpret_cast<const sockaddr_in*>(address->Address.lpSockaddr);
                std::memcpy(a.ip.data(),&v4->sin_addr,4); a.has_ip=true; break;
            }
        }
        out.push_back(a);
    }
    return out;
}

static Adapter select_adapter(const std::string& name) {
    const auto wanted=lower_ascii(name);
    for(const auto& a:adapters()) {
        if(wanted==lower_ascii(a.friendly) || wanted==lower_ascii(a.adapter_name) ||
           wanted==lower_ascii(a.pcap_name) || wanted==std::to_string(a.index)) return a;
    }
    throw std::runtime_error("interface not found; use --list-interfaces and copy its name exactly");
}

struct Device {
    PcapApi api; pcap_t* capture=nullptr; Mac mac{}; IP ip{}; ULONG index=0;
    std::string friendly,pcap_name;
    explicit Device(const std::string& name) {
        const auto adapter=select_adapter(name);
        require(adapter.type==IF_TYPE_ETHERNET_CSMACD,"interface must be a physical Ethernet adapter");
        require(adapter.status==IfOperStatusUp,"interface must be connected and up");
        require(adapter.has_ip,"interface needs an IPv4 address");
        require(adapter.mac!=Mac{},"interface needs a six-byte MAC address");
        friendly=adapter.friendly; pcap_name=adapter.pcap_name; index=adapter.index;
        mac=adapter.mac; ip=adapter.ip;
        char error[256]{};
        capture=api.open_live(pcap_name.c_str(),65536,1,100,error);
        if(!capture) throw std::runtime_error(std::string("Npcap open failed: ")+error);
    }
    ~Device() { if(capture) api.close(capture); }
    Device(const Device&)=delete; Device& operator=(const Device&)=delete;
    void send(const Bytes& v) {
        if(api.sendpacket(capture,v.data(),int(v.size()))!=0)
            throw std::runtime_error(std::string("Npcap send failed: ")+api.geterr(capture));
    }
    bool receive(Bytes& out) {
        pcap_pkthdr* header=nullptr; const unsigned char* bytes=nullptr;
        const int result=api.next_ex(capture,&header,&bytes);
        if(result==0) return false;
        if(result<0) throw std::runtime_error(std::string("Npcap receive failed: ")+api.geterr(capture));
        if(!header || !bytes || header->caplen==0) return false;
        out.assign(bytes,bytes+header->caplen); return true;
    }
};

static void bind_device(SOCKET fd,const Device& device) {
    const DWORD network_index=htonl(device.index);
    syscheck(setsockopt(fd,IPPROTO_IP,IP_UNICAST_IF,
        reinterpret_cast<const char*>(&network_index),sizeof network_index)==0,
        "bind socket to physical interface");
}

static void nonblocking(SOCKET fd) {
    u_long enabled=1; syscheck(ioctlsocket(fd,FIONBIO,&enabled)==0,"set nonblocking socket");
}

// A-only DNS query to the configured campus resolvers; bounded timeout per server.
// DNS name compression is skipped safely because only A RDATA is consumed.
static size_t skip_name(const Bytes& v,size_t p) {
    for(unsigned count=0;count<128;++count) {
        require(p<v.size(),"truncated DNS name"); unsigned n=v[p++];
        if(n==0) return p;
        if((n&0xc0)==0xc0) { require(p<v.size(),"truncated DNS pointer"); return p+1; }
        require(n<=63 && p+n<=v.size(),"invalid DNS label"); p+=n;
    }
    throw std::runtime_error("DNS name too long");
}
static std::pair<IP,IP> resolve(const Config& c,const Device& device) {
    IP literal{};
    if(inet_pton(AF_INET,c.host.c_str(),literal.data())==1) return {literal,c.dns.front()};
    std::random_device rng; const uint16_t id=uint16_t(rng());
    Bytes query; put_be16(query,id); append(query,unhex("01000001000000000000"));
    std::stringstream labels(c.host); std::string label;
    while(std::getline(labels,label,'.')) {
        require(!label.empty() && label.size()<=63,"invalid DNS hostname");
        query.push_back(uint8_t(label.size())); append(query,label);
    }
    query.push_back(0); append(query,unhex("00010001"));
    for(const auto& dns:c.dns) {
        if(stop_requested()) throw std::runtime_error("interrupted");
        try {
            Fd fd(socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP));
            syscheck(fd.n!=INVALID_SOCKET,"DNS socket"); bind_device(fd.n,device);
            auto local=address(device.ip,0),remote=address(dns,53);
            syscheck(bind(fd.n,reinterpret_cast<sockaddr*>(&local),sizeof local)==0,"DNS bind");
            syscheck(connect(fd.n,reinterpret_cast<sockaddr*>(&remote),sizeof remote)==0,"DNS connect");
            syscheck(send(fd.n,reinterpret_cast<const char*>(query.data()),int(query.size()),0)==int(query.size()),"DNS send");
            fd_set readable; FD_ZERO(&readable); FD_SET(fd.n,&readable); timeval timeout{2,0};
            const int ready=select(0,&readable,nullptr,nullptr,&timeout); if(ready<=0) continue;
            Bytes reply(4096); const int n=recv(fd.n,reinterpret_cast<char*>(reply.data()),int(reply.size()),0);
            if(n<12) continue;
            reply.resize(size_t(n));
            require(be16(reply,0)==id && (be16(reply,2)&0xfa0f)==0x8000,"invalid DNS response");
            size_t offset=12;
            for(unsigned i=0;i<be16(reply,4);++i) { offset=skip_name(reply,offset)+4; require(offset<=reply.size(),"short DNS question"); }
            for(unsigned i=0;i<be16(reply,6);++i) {
                offset=skip_name(reply,offset); require(offset+10<=reply.size(),"short DNS answer");
                unsigned type=be16(reply,offset),cls=be16(reply,offset+2),len=be16(reply,offset+8); offset+=10;
                require(offset+len<=reply.size(),"short DNS data");
                if(type==1 && cls==1 && len==4) { IP ip{}; std::copy_n(reply.begin()+offset,4,ip.begin()); return {ip,dns}; }
                offset+=len;
            }
        } catch(const std::exception& e) { log(std::string("DNS: ")+e.what()); }
    }
    throw std::runtime_error("cannot resolve server using configured DNS servers");
}
struct Pending { Bytes packet; Time deadline{}; int attempts=0; bool active=false; };
struct SleepUntilMorning:std::runtime_error { SleepUntilMorning():std::runtime_error("server prohibits access at this time"){} };
enum class UdpStage { Off, AliveResponse, InfoResponse, StartupDelay, StartupHeartbeat, Ready, HeartbeatAlive, Heartbeat2, Heartbeat4 };

class Session {
    const Config& c; Device dev; Fd udp;
    IP auth_ip{},udp_ip{};
    Mac client_mac{};
    Mac server=multicast; Bytes cached_identity,digest,udp_digest,flux,info,random;
    IP dns{}; uint8_t counter=0; bool authenticated=false;
    UdpStage stage=UdpStage::Off; Pending ep,up;
    Time last_eap=Clock::now(),next_startup{},next_alive{},udp_deadline{};
    int heartbeat_failures=0;
    std::mt19937 rng{std::random_device{}()};
    void send_eap(const Bytes& packet,bool retry) {
        dev.send(packet); if(retry) ep={packet,Clock::now()+std::chrono::milliseconds(c.interval),0,true};
    }
    void send_udp(const Bytes& packet) {
        syscheck(send(udp.n,reinterpret_cast<const char*>(packet.data()),int(packet.size()),0)==int(packet.size()),"send UDP");
        up={packet,Clock::now()+std::chrono::milliseconds(c.interval),0,true};
    }
    void open_udp() {
        const auto resolved=resolve(c,dev); dns=resolved.second;
        udp.reset(socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP));
        syscheck(udp.n!=INVALID_SOCKET,"UDP socket"); bind_device(udp.n,dev);
        bool bound=false;
        const unsigned first_port=c.udp_local_port ? unsigned(c.udp_local_port) : 36144;
        const unsigned last_port=c.udp_local_port ? unsigned(c.udp_local_port) : 65535;
        for(unsigned port=first_port;port<=last_port;++port) {
            auto a=address(dev.ip,uint16_t(port));
            if(bind(udp.n,reinterpret_cast<sockaddr*>(&a),sizeof a)==0) { bound=true; break; }
            if(WSAGetLastError()!=WSAEADDRINUSE) syscheck(false,"UDP bind");
        }
        require(bound,"no available local UDP port"); auto remote=address(resolved.first,61440);
        syscheck(connect(udp.n,reinterpret_cast<sockaddr*>(&remote),sizeof remote)==0,"UDP connect");
        nonblocking(udp.n);
        udp_digest=digest; counter=0; stage=UdpStage::AliveResponse;
        log("EAP authenticated; UDP server="+ip_text(resolved.first)+":61440"); send_udp(misc_alive());
    }
    void eap_packet(const Bytes& frame) {
        Eap e;
        try { e=parse_eap(frame); } catch(const std::exception&) { return; }
        if(e.dst!=client_mac || e.src==client_mac) return;
        if(server!=multicast && server!=e.src) return;
        if(e.code==1 && e.type==1) {
            ep.active=false; last_eap=Clock::now();
            if(!cached_identity.empty()) {
                cached_identity[19]=e.id; send_eap(cached_identity,!authenticated); log("EAP Identity heartbeat");
            } else {
                if(e.id>1) throw std::runtime_error("server requests a fresh EAP session");
                server=e.src; cached_identity=identity(server,client_mac,e.id,c.user,auth_ip);
                send_eap(cached_identity,true); log("EAP Identity response");
            }
        } else if(e.code==1 && e.type==4) {
            if(cached_identity.empty() || e.payload.empty() || e.payload[0]==0 || size_t(e.payload[0])+1>e.payload.size()) return;
            if(authenticated) {
                authenticated=false; udp.reset(); up.active=false; info.clear(); stage=UdpStage::Off;
            }
            ep.active=false; digest=md5_response(e.id,c.password,slice(e.payload,1,e.payload[0]));
            const uint8_t version=c.profile=="windows-31" ? 0x31 : 0x2a;
            send_eap(challenge_response(server,client_mac,e.id,c.user,auth_ip,digest,version),true); log("EAP MD5 response");
        } else if(e.code==3 && !digest.empty()) {
            ep.active=false; last_eap=Clock::now();
            if(!authenticated) { authenticated=true; open_udp(); }
        } else if(e.code==4) throw std::runtime_error("EAP authentication failed");
        else if(e.code==1 && e.type==2) {
            std::string message(e.payload.begin(),e.payload.end()); log("EAP notification: "+message);
            if(message=="Authentication Fail ErrCode=16") throw SleepUntilMorning();
            throw std::runtime_error("EAP server rejected authentication");
        }
    }
    void udp_packet(const Bytes& v) {
        if(v.size()<5) return;
        if(v[0]==0x4d) { log("UDP server message (hex/GB18030): "+hex(slice(v,4,v.size()-4))); return; }
        if(v[0]!=7) return;
        if(v[4]==2 && stage==UdpStage::AliveResponse && v.size()>=12) {
            flux=slice(v,8,4);
            auto packet=c.profile=="windows-31"
                ? misc_info_windows31(client_mac,udp_ip,c.user,c.hostname,dns,*c.gateway,c.dns[1],flux,c.udp_trailer)
                : misc_info(client_mac,udp_ip,c.user,c.hostname,dns,flux);
            std::copy_n(packet.begin()+24,4,udp_digest.begin()); stage=UdpStage::InfoResponse;
            send_udp(packet); log("UDP MiscInfo sent");
        } else if(v[4]==4 && stage==UdpStage::InfoResponse && v.size()>=32) {
            up.active=false; info=decrypt_info(slice(v,16,16)); log("UDP session established");
            if(c.profile=="windows-31") {
                random={uint8_t(rng()),uint8_t(rng())}; stage=UdpStage::StartupDelay;
                next_startup=Clock::now()+std::chrono::milliseconds(1400);
                udp_deadline=Clock::now()+std::chrono::seconds(c.udp_timeout*(c.retry+2));
            } else {
                stage=UdpStage::Ready; next_alive=Clock::now();
                udp_deadline=Clock::now()+std::chrono::seconds(c.udp_timeout*(c.retry+2));
            }
        } else if(v[4]==0x0b && v.size()>=6 && v[5]==6 && stage==UdpStage::StartupHeartbeat) {
            up.active=false; stage=UdpStage::Ready;
            next_alive=Clock::now()+std::chrono::seconds(c.heartbeat_interval);
            udp_deadline=next_alive+std::chrono::seconds(c.udp_timeout*(c.retry+2));
            log("UDP startup heartbeat complete");
        } else if(v[4]==6 && stage==UdpStage::HeartbeatAlive) {
            random={uint8_t(rng()),uint8_t(rng())}; stage=UdpStage::Heartbeat2;
            send_udp(heartbeat(1,++counter,random,flux,udp_ip));
        } else if(v[4]==0x0b && v.size()>=20 && v[5]==2 && stage==UdpStage::Heartbeat2) {
            flux=slice(v,16,4); stage=UdpStage::Heartbeat4; send_udp(heartbeat(3,++counter,random,flux,udp_ip));
        } else if(v[4]==0x0b && v.size()>=6 && v[5]==4 && stage==UdpStage::Heartbeat4) {
            up.active=false; stage=UdpStage::Ready; heartbeat_failures=0;
            udp_deadline=next_alive+std::chrono::seconds(c.udp_timeout*(c.retry+2)); log("UDP heartbeat complete");
        }
    }
    void retry(Pending& p,bool eap) {
        if(!p.active || Clock::now()<p.deadline) return;
        if(p.attempts>=c.retry) {
            const bool periodic=!eap && (stage==UdpStage::HeartbeatAlive ||
                stage==UdpStage::Heartbeat2 || stage==UdpStage::Heartbeat4);
            if(periodic && ++heartbeat_failures<3) {
                p.active=false; stage=UdpStage::Ready;
                next_alive=Clock::now()+std::chrono::seconds(c.heartbeat_interval);
                udp_deadline=next_alive+std::chrono::seconds(c.udp_timeout*(c.retry+2));
                log("UDP heartbeat reply timeout; ignored ("+std::to_string(heartbeat_failures)+"/3)");
                return;
            }
            throw std::runtime_error(eap?"EAP reply timeout":"UDP reply timeout");
        }
        if(eap) dev.send(p.packet);
        else syscheck(send(udp.n,reinterpret_cast<const char*>(p.packet.data()),int(p.packet.size()),0)==int(p.packet.size()),"resend UDP");
        ++p.attempts; p.deadline=Clock::now()+std::chrono::milliseconds(c.interval); log(eap?"EAP retry":"UDP retry");
    }
public:
    explicit Session(const Config& cfg):c(cfg),dev(c.iface),auth_ip(c.ip.value_or(dev.ip)),
        udp_ip(c.udp_ip.value_or(dev.ip)),client_mac(c.mac.value_or(dev.mac)) {
        log("Interface="+c.iface+" LocalIPv4="+ip_text(dev.ip)+" AuthIPv4="+ip_text(auth_ip)+
            " UdpIPv4="+ip_text(udp_ip)+" MAC="+hex(Bytes(client_mac.begin(),client_mac.end()))+
            " Profile="+c.profile);
    }
    ~Session() {
        // Best-effort explicit logoff on shutdown/reconnect. Check mode sends nothing.
        if(!c.check) try { dev.send(ethernet(multicast,client_mac,2)); } catch(...) {}
    }
    void run(Time stop_at) {
        if(c.check) { log("Raw socket open OK; no packets sent"); return; }
        send_eap(ethernet(multicast,client_mac,2),false);
        const auto start_at=Clock::now()+std::chrono::seconds(2); bool started=false;
        while(!stop_requested() && Clock::now()<stop_at) {
            const auto now=Clock::now();
            if(!started && now>=start_at) { started=true; send_eap(ethernet(multicast,client_mac,1),true); log("EAP Start"); }
            if(started) { retry(ep,true); retry(up,false); }
            if(authenticated && now-last_eap>std::chrono::seconds(c.eap_timeout*(c.retry+2)))
                throw std::runtime_error("EAP Identity heartbeat timeout");
            if(!info.empty() && now>udp_deadline) throw std::runtime_error("UDP heartbeat timeout");
            if(stage==UdpStage::StartupDelay && now>=next_startup) {
                stage=UdpStage::StartupHeartbeat;
                send_udp(startup_heartbeat_windows31(random)); log("UDP startup heartbeat sent");
            }
            if(stage==UdpStage::Ready && now>=next_alive) {
                send_udp(alive(udp_digest,info,uint16_t(std::time(nullptr)))); stage=UdpStage::HeartbeatAlive;
                next_alive=now+std::chrono::seconds(c.heartbeat_interval);
            }
            Bytes packet;
            if(dev.receive(packet) && started) eap_packet(packet);
            if(udp.n!=INVALID_SOCKET) {
                for(;;) {
                    packet.assign(4096,0);
                    const int n=recv(udp.n,reinterpret_cast<char*>(packet.data()),int(packet.size()),0);
                    if(n==SOCKET_ERROR) {
                        const int error=WSAGetLastError();
                        if(error==WSAEWOULDBLOCK) break;
                        throw std::runtime_error("UDP receive failed (Winsock error "+std::to_string(error)+")");
                    }
                    if(n==0) break;
                    packet.resize(size_t(n)); if(started) udp_packet(packet);
                }
            }
        }
    }
};
static void wait_until(Time deadline,Time stop_at) {
    while(!stop_requested() && Clock::now()<deadline && Clock::now()<stop_at) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
static Time next_morning(const Config& c) {
    const auto now=std::time(nullptr); std::tm local{}; localtime_s(&local,&now);
    local.tm_hour=c.wake_hour; local.tm_min=c.wake_minute; local.tm_sec=0; local.tm_isdst=-1;
    auto target=std::mktime(&local);
    if(target<=now) { ++local.tm_mday; local.tm_isdst=-1; target=std::mktime(&local); }
    require(target!=-1,"cannot calculate next reconnect time"); return Clock::now()+std::chrono::seconds(target-now);
}
int main(int argc,char** argv) {
    try {
        Wsa wsa;
        for(int i=1;i<argc;++i) {
            if(std::string(argv[i])=="--help") { usage(); return 0; }
            if(std::string(argv[i])=="--list-interfaces") {
                for(const auto& a:adapters()) {
                    if(a.type!=IF_TYPE_ETHERNET_CSMACD) continue;
                    std::cout<<a.index<<" \""<<a.friendly<<"\""
                        <<" IPv4="<<(a.has_ip?ip_text(a.ip):"-")
                        <<" MAC="<<hex(Bytes(a.mac.begin(),a.mac.end()))
                        <<" State="<<(a.status==IfOperStatusUp?"up":"down")
                        <<" Npcap="<<a.pcap_name<<"\n";
                }
                return 0;
            }
        }
        const Config c=config(argc,argv); std::signal(SIGINT,signal_stop); std::signal(SIGTERM,signal_stop);
        if(!c.stop_event_name.empty()) {
            stop_event=CreateEventA(nullptr,TRUE,FALSE,c.stop_event_name.c_str());
            if(!stop_event) throw std::runtime_error("cannot create GUI stop event");
        }
        const auto stop_at=c.run_seconds?Clock::now()+std::chrono::seconds(c.run_seconds):Time::max();
        while(!stop_requested() && Clock::now()<stop_at) {
            auto resume=Clock::now()+std::chrono::seconds(c.reconnect);
            try { Session session(c); session.run(stop_at); return 0; }
            catch(const SleepUntilMorning& e) {
                log(e.what()); if(c.once || c.check) return 1; resume=next_morning(c);
            } catch(const std::exception& e) {
                log(std::string("Session error: ")+e.what()); if(c.once || c.check) return 1;
                resume=Clock::now()+std::chrono::seconds(c.reconnect);
            }
            log("Waiting before reconnect"); wait_until(resume,stop_at);
        }
        if(stop_event) { CloseHandle(stop_event); stop_event=nullptr; }
        return 0;
    } catch(const std::exception& e) { std::cerr<<"Error: "<<e.what()<<"\n"; return 1; }
}
