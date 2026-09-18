// Linux C++17 client port. Packet algorithms are in protocol.hpp.
#include "protocol.hpp"
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netpacket/packet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
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

using namespace scut;
using Clock=std::chrono::steady_clock;
using Time=Clock::time_point;
static volatile std::sig_atomic_t stopping=0;
static void signal_stop(int) { stopping=1; }
static void log(const std::string& message) { std::cout<<message<<std::endl; }
static void syscheck(bool ok,const std::string& what) {
    if(!ok) throw std::runtime_error(what+": "+std::strerror(errno));
}
struct Fd {
    int n=-1;
    Fd()=default; explicit Fd(int value):n(value){}
    Fd(const Fd&)=delete; Fd& operator=(const Fd&)=delete;
    ~Fd(){ reset(); }
    void reset(int value=-1){ if(n>=0) close(n); n=value; }
};
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
    std::string profile="windows-31";
    std::optional<IP> ip,udp_ip,gateway;
    std::optional<Mac> mac;
    Bytes udp_trailer=unhex("2001025030007004aa0cb7dee93f3c65");
    std::vector<IP> dns{parse_ip("202.38.193.33"),parse_ip("222.201.130.30"),parse_ip("202.112.17.33"),parse_ip("222.201.130.33")};
    int retry=2,interval=5000,reconnect=15,eap_timeout=60,udp_timeout=12,heartbeat_interval=300,wake_hour=7,wake_minute=0,run_seconds=0,udp_local_port=0;
    bool once=false,check=false;
};
static void usage() {
    std::cout<<"drcom4scut-cpp (Linux, IPv4)\n"
        "  --config FILE          key=value config (NOT YAML)\n"
        "  --interface NAME       Ethernet interface, e.g. eth0\n"
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
        "  --heartbeat-interval SEC  interval between established UDP heartbeats\n"
        "  --list-interfaces      list interfaces, no authentication\n"
        "  --check-interface      open raw socket then exit, no packets sent\n"
        "  --once                 exit instead of reconnecting after failure\n"
        "  --run-seconds SEC      bounded run (0 means unlimited)\n"
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
    require(!c.iface.empty() && c.iface.size()<IFNAMSIZ,"specify --interface NAME");
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
struct Device {
    Fd raw; Mac mac{}; IP ip{}; int index=0;
    explicit Device(const std::string& name) {
        Fd control(socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0)); syscheck(control.n>=0,"control socket");
        ifreq req{}; std::strncpy(req.ifr_name,name.c_str(),IFNAMSIZ-1);
        syscheck(ioctl(control.n,SIOCGIFINDEX,&req)==0,"interface index"); index=req.ifr_ifindex;
        syscheck(ioctl(control.n,SIOCGIFFLAGS,&req)==0,"interface flags");
        require((req.ifr_flags&IFF_UP)!=0 && (req.ifr_flags&IFF_LOOPBACK)==0,"interface must be up and non-loopback");
        syscheck(ioctl(control.n,SIOCGIFHWADDR,&req)==0,"interface MAC");
        require(req.ifr_hwaddr.sa_family==ARPHRD_ETHER,"interface must use Ethernet framing");
        std::memcpy(mac.data(),req.ifr_hwaddr.sa_data,6);
        syscheck(ioctl(control.n,SIOCGIFADDR,&req)==0,"interface needs an IPv4 address");
        std::memcpy(ip.data(),&reinterpret_cast<sockaddr_in*>(&req.ifr_addr)->sin_addr,4);
        raw.reset(socket(AF_PACKET,SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,htons(0x888e)));
        syscheck(raw.n>=0,"raw Ethernet socket (root or CAP_NET_RAW required)");
        sockaddr_ll a{}; a.sll_family=AF_PACKET; a.sll_protocol=htons(0x888e); a.sll_ifindex=index;
        syscheck(bind(raw.n,reinterpret_cast<sockaddr*>(&a),sizeof a)==0,"bind Ethernet socket");
    }
    void send(const Bytes& v) {
        sockaddr_ll a{}; a.sll_family=AF_PACKET; a.sll_protocol=htons(0x888e); a.sll_ifindex=index; a.sll_halen=6;
        std::copy_n(v.begin(),6,a.sll_addr);
        syscheck(sendto(raw.n,v.data(),v.size(),0,reinterpret_cast<sockaddr*>(&a),sizeof a)==ssize_t(v.size()),"send Ethernet");
    }
};
static void bind_device(int fd,const std::string& name) {
    syscheck(setsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,name.c_str(),socklen_t(name.size()+1))==0,"bind socket to interface");
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
        if(stopping) throw std::runtime_error("interrupted");
        try {
            Fd fd(socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0)); syscheck(fd.n>=0,"DNS socket"); bind_device(fd.n,c.iface);
            auto local=address(device.ip,0),remote=address(dns,53);
            syscheck(bind(fd.n,reinterpret_cast<sockaddr*>(&local),sizeof local)==0,"DNS bind");
            syscheck(connect(fd.n,reinterpret_cast<sockaddr*>(&remote),sizeof remote)==0,"DNS connect");
            syscheck(send(fd.n,query.data(),query.size(),0)==ssize_t(query.size()),"DNS send");
            pollfd p{fd.n,POLLIN,0}; if(poll(&p,1,2000)<=0) continue;
            Bytes reply(4096); ssize_t n=recv(fd.n,reply.data(),reply.size(),0); if(n<12) continue; reply.resize(size_t(n));
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
    std::mt19937 rng{std::random_device{}()};
    void send_eap(const Bytes& packet,bool retry) {
        dev.send(packet); if(retry) ep={packet,Clock::now()+std::chrono::milliseconds(c.interval),0,true};
    }
    void send_udp(const Bytes& packet) {
        syscheck(send(udp.n,packet.data(),packet.size(),0)==ssize_t(packet.size()),"send UDP");
        up={packet,Clock::now()+std::chrono::milliseconds(c.interval),0,true};
    }
    void open_udp() {
        const auto resolved=resolve(c,dev); dns=resolved.second;
        udp.reset(socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0)); syscheck(udp.n>=0,"UDP socket"); bind_device(udp.n,c.iface);
        bool bound=false;
        const unsigned first_port=c.udp_local_port ? unsigned(c.udp_local_port) : 36144;
        const unsigned last_port=c.udp_local_port ? unsigned(c.udp_local_port) : 65535;
        for(unsigned port=first_port;port<=last_port;++port) {
            auto a=address(dev.ip,uint16_t(port));
            if(bind(udp.n,reinterpret_cast<sockaddr*>(&a),sizeof a)==0) { bound=true; break; }
            if(errno!=EADDRINUSE) syscheck(false,"UDP bind");
        }
        require(bound,"no available local UDP port"); auto remote=address(resolved.first,61440);
        syscheck(connect(udp.n,reinterpret_cast<sockaddr*>(&remote),sizeof remote)==0,"UDP connect");
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
            up.active=false; stage=UdpStage::Ready;
            udp_deadline=next_alive+std::chrono::seconds(c.udp_timeout*(c.retry+2)); log("UDP heartbeat complete");
        }
    }
    void retry(Pending& p,bool eap) {
        if(!p.active || Clock::now()<p.deadline) return;
        if(p.attempts>=c.retry) throw std::runtime_error(eap?"EAP reply timeout":"UDP reply timeout");
        if(eap) dev.send(p.packet);
        else syscheck(send(udp.n,p.packet.data(),p.packet.size(),0)==ssize_t(p.packet.size()),"resend UDP");
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
        while(!stopping && Clock::now()<stop_at) {
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
            pollfd fds[2]{{dev.raw.n,POLLIN,0},{udp.n,POLLIN,0}};
            int ready=poll(fds,2,100); if(ready<0 && errno==EINTR) continue; syscheck(ready>=0,"poll");
            for(int i=0;i<2;++i) {
                if(fds[i].revents&(POLLERR|POLLHUP|POLLNVAL)) throw std::runtime_error("network socket error");
                if(!(fds[i].revents&POLLIN)) continue;
                Bytes packet(4096); ssize_t n=recv(fds[i].fd,packet.data(),packet.size(),0);
                if(n<0 && (errno==EAGAIN || errno==EINTR)) continue;
                syscheck(n>=0,"receive"); packet.resize(size_t(n));
                if(started) { if(i==0) eap_packet(packet); else udp_packet(packet); }
            }
        }
    }
};
static void wait_until(Time deadline,Time stop_at) {
    while(!stopping && Clock::now()<deadline && Clock::now()<stop_at) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
static Time next_morning(const Config& c) {
    const auto now=std::time(nullptr); std::tm local{}; localtime_r(&now,&local);
    local.tm_hour=c.wake_hour; local.tm_min=c.wake_minute; local.tm_sec=0; local.tm_isdst=-1;
    auto target=std::mktime(&local);
    if(target<=now) { ++local.tm_mday; local.tm_isdst=-1; target=std::mktime(&local); }
    require(target!=-1,"cannot calculate next reconnect time"); return Clock::now()+std::chrono::seconds(target-now);
}
int main(int argc,char** argv) {
    try {
        for(int i=1;i<argc;++i) {
            if(std::string(argv[i])=="--help") { usage(); return 0; }
            if(std::string(argv[i])=="--list-interfaces") {
                struct if_nameindex* list=if_nameindex(); syscheck(list!=nullptr,"list interfaces");
                for(auto p=list;p->if_index;++p) std::cout<<p->if_index<<" "<<p->if_name<<"\n";
                if_freenameindex(list); return 0;
            }
        }
        const Config c=config(argc,argv); std::signal(SIGINT,signal_stop); std::signal(SIGTERM,signal_stop);
        const auto stop_at=c.run_seconds?Clock::now()+std::chrono::seconds(c.run_seconds):Time::max();
        while(!stopping && Clock::now()<stop_at) {
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
        return 0;
    } catch(const std::exception& e) { std::cerr<<"Error: "<<e.what()<<"\n"; return 1; }
}
