#include "../protocol.hpp"
#include <iostream>
using namespace scut;
static unsigned checks=0;
static void expect(bool ok,const char* name) {
    ++checks; if(!ok) throw std::runtime_error(name);
}
template<class F> static void rejects(F f) {
    bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; }
    expect(rejected,"invalid packet was accepted");
}
int main() {
    try {
        expect(hex(md5_response(0,"qwert12345",unhex("ff62b079ca26d283ca26d28300000000")))==
               "313a3758ad589ce03dc6af0371c31239","upstream MD5 vector");
        Mac mac{0xb0,0x25,0xaa,0x28,0x6d,0xb9},server{2,0,0,0,0,2}; IP ip{125,217,254,225},dns{202,38,193,33};
        auto id=identity(server,mac,1,"student",ip); auto parsed=parse_eap(id);
        expect(id.size()==96 && parsed.code==2 && parsed.type==1 && parsed.id==1,"Identity headers");
        expect(hex(parsed.payload)=="73747564656e7400446100007dd9fee1","Identity body");
        auto digest=md5_response(0,"qwert12345",unhex("ff62b079ca26d283ca26d28300000000"));
        auto md=parse_eap(challenge_response(server,mac,0,"student",ip,digest));
        expect(hex(md.payload)=="10313a3758ad589ce03dc6af0371c3123973747564656e740044612a007dd9fee1","MD5 response body");
        auto md31=parse_eap(challenge_response(server,mac,0,"student",ip,digest,0x31));
        expect(hex(md31.payload)=="10313a3758ad589ce03dc6af0371c3123973747564656e7400446131007dd9fee1","Windows-31 MD5 response body");
        // Fixed capture from the existing Rust packet tests: checks byte offsets and checksum together.
        const std::string golden=
          "0701f400030cb025aa286db97dd9fee10222002a3bab4e044af8a726000000003230313833363433313135345365614c6f6f6e67000000000000000000000000000000000000000000000000ca26c12100000000ca7011210000000000000000940000000600000002000000f0230000020000004472434f4d0096022a0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000034656238316663303438613535383562376466653137383331353532343161333238623130336336000000000000000000000000000000000000000000000000";
        Bytes capture=unhex(golden); set_le32(capture,24,0x01312fc7); checksum32(capture);
        expect(hex(capture)==golden,"upstream checksum32 capture");
        auto info=misc_info(mac,ip,"201836431154","SeaLoong",dns,unhex("3bab4e04"));
        // Captured packet has dns2=202.112.17.33; the original running client writes dns2=0.
        std::copy_n(unhex("ca701121").begin(),4,info.begin()+84);
        set_le32(info,24,0x01312fc7); checksum32(info);
        expect(hex(info)==golden,"entire MiscInfo packet matches upstream capture");
        Mac winmac{0xbc,0xfc,0xe7,0xc6,0x36,0xf1}; IP winip{10,40,8,114};
        IP gateway{10,40,8,254},dns2{202,112,17,33};
        auto wininfo=misc_info_windows31(winmac,winip,"student00000","bird",dns,gateway,dns2,
                                         unhex("4e44f827"),unhex("2001025030007004aa0cb7dee93f3c65"));
        const std::string win_golden=
          "07010401030cbcfce7c636f10a280872022200314e44f82788a9069e0000000073747564656e7430303030306269726400000000000000000000000000000000000000000000000000000000ca26c1210a2808feca701121000000000000000000000000000000000000000000000000000000004472434f4d0096023100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000623162353464613165623634656431343539323833306637663839356333373364353037343732610000000000000000000000000000000000000000000000002001025030007004aa0cb7dee93f3c65";
        expect(wininfo.size()==260 && hex(wininfo)==win_golden,"Windows-31 MiscInfo capture profile");
        expect(hex(heartbeat(3,0x56,unhex("b919"),unhex("67513f04"),ip))==
               "075628000b03dc02b91900000000000067513f0400000000b6e062007dd9fee10000000000000000","upstream Heartbeat3 capture");
        expect(hex(decrypt_info(unhex("4439d8edac314b07dd8c5f3bef0f04d8")))==
               "4472636fca26d283dd197dd9fee1016c","upstream info decryption");
        auto a=alive(digest,Bytes(16,0x42),0x1234);
        expect(a.size()==38 && a[0]==255 && a[36]==0x34 && a[37]==0x12,"Alive layout and time endianness");
        expect(misc_alive()==unhex("0700080001000000"),"MiscAlive layout");
        expect(hex(startup_heartbeat_windows31(unhex("c201")))==
               "070028000b010f27c201000000000000000000000000000000000000000000000000000000000000",
               "Windows-31 startup heartbeat capture");
        for(size_t n=0;n<23;++n) rejects([&]{parse_eap(slice(id,0,n));});
        auto broken=id; broken[17]=255; rejects([&]{parse_eap(broken);});
        rejects([&]{challenge_response(server,mac,1,"student",ip,Bytes(15));});
        rejects([&]{misc_info(mac,ip,"student","host",dns,Bytes(3));});
        rejects([&]{heartbeat(3,0,Bytes(1),Bytes(4),ip);});
        rejects([&]{decrypt_info(Bytes(15));});
        for(unsigned n=0;n<256;++n) {
            auto p=heartbeat(1,uint8_t(n+1),Bytes(2),Bytes(4),ip);
            expect(p.size()==40 && p[1]==uint8_t(n+1),"heartbeat counter wrapping");
        }
        std::cout<<"PASS: "<<checks<<" protocol checks\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<"\n"; return 1; }
}
