#pragma once
// Protocol port of SeaLoong/drcom4scut src/eap.rs and src/udp/packet.rs.
// See LICENSE, NOTICE.md and README.md for provenance.
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <openssl/evp.h>

namespace scut {
using Bytes = std::vector<uint8_t>;
using Mac = std::array<uint8_t, 6>;
using IP = std::array<uint8_t, 4>;
inline const Mac multicast{1, 0x80, 0xc2, 0, 0, 3};
inline void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> inline void append(Bytes& out, const T& data) {
    out.insert(out.end(), data.begin(), data.end());
}
inline Bytes unhex(const std::string& s) {
    require(s.size() % 2 == 0, "odd hex length");
    Bytes out;
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return unsigned(c - '0');
        if (c >= 'a' && c <= 'f') return unsigned(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return unsigned(c - 'A' + 10);
        throw std::runtime_error("invalid hex digit");
    };
    for (size_t i = 0; i < s.size(); i += 2)
        out.push_back(uint8_t(digit(s[i]) * 16 + digit(s[i + 1])));
    return out;
}
inline std::string hex(const Bytes& v) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (auto b : v) { out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
}
inline uint16_t be16(const Bytes& v, size_t p) {
    require(p + 2 <= v.size(), "truncated uint16");
    return uint16_t((unsigned(v[p]) << 8) | v[p + 1]);
}
inline uint32_t le32(const Bytes& v, size_t p) {
    require(p + 4 <= v.size(), "truncated uint32");
    return uint32_t(v[p]) | uint32_t(v[p+1]) << 8 | uint32_t(v[p+2]) << 16 | uint32_t(v[p+3]) << 24;
}
inline void put_be16(Bytes& v, uint16_t n) { v.push_back(uint8_t(n >> 8)); v.push_back(uint8_t(n)); }
inline void set_le32(Bytes& v, size_t p, uint32_t n) {
    require(p + 4 <= v.size(), "invalid uint32 offset");
    for (unsigned i = 0; i < 4; ++i) v[p+i] = uint8_t(n >> (8*i));
}
inline Bytes slice(const Bytes& v, size_t p, size_t n) {
    require(p <= v.size() && n <= v.size() - p, "truncated packet payload");
    return Bytes(v.begin() + p, v.begin() + p + n);
}
inline Bytes md5_response(uint8_t id, const std::string& password, const Bytes& challenge) {
    Bytes input{id}; append(input, password); append(input, challenge);
    Bytes result(EVP_MAX_MD_SIZE); unsigned length = 0;
    require(EVP_Digest(input.data(), input.size(), result.data(), &length, EVP_md5(), nullptr) == 1,
            "OpenSSL MD5 unavailable (required by this protocol)");
    result.resize(length); require(length == 16, "unexpected MD5 length"); return result;
}
inline Bytes ethernet(const Mac& dst, const Mac& src, uint8_t type, const Bytes& body = {}) {
    require(body.size() <= 65535, "EAPOL payload too large");
    Bytes out; append(out, dst); append(out, src);
    append(out, Bytes{0x88, 0x8e, 1, type}); put_be16(out, uint16_t(body.size())); append(out, body);
    if (out.size() < 96) out.resize(96);
    return out;
}
inline Bytes response(const Mac& dst, const Mac& src, uint8_t id, uint8_t type, const Bytes& payload) {
    require(payload.size() <= 65530, "EAP payload too large");
    Bytes body{2, id}; put_be16(body, uint16_t(payload.size()+5)); body.push_back(type); append(body,payload);
    return ethernet(dst,src,0,body);
}
inline Bytes identity(const Mac& dst, const Mac& src, uint8_t id, const std::string& user, const IP& ip) {
    Bytes payload; append(payload,user); append(payload,unhex("0044610000")); append(payload,ip);
    return response(dst,src,id,1,payload);
}
inline Bytes challenge_response(const Mac& dst, const Mac& src, uint8_t id, const std::string& user,
                                const IP& ip, const Bytes& digest, uint8_t client_version=0x2a) {
    require(digest.size()==16,"MD5 must be 16 bytes");
    Bytes payload{16}; append(payload,digest); append(payload,user);
    append(payload,Bytes{0x00,0x44,0x61,client_version,0x00}); append(payload,ip);
    return response(dst,src,id,4,payload);
}
struct Eap {
    Mac dst{}, src{}; uint8_t code{}, id{}, type{}; Bytes payload;
};
inline Eap parse_eap(const Bytes& frame) {
    require(frame.size()>=22,"short EAP frame");
    require(be16(frame,12)==0x888e && frame[14]==1 && frame[15]==0,"not EAPOL v1 EAP");
    const size_t n=be16(frame,16), len=be16(frame,20);
    require(n>=4 && len==n && frame.size()>=18+n,"invalid EAP lengths");
    Eap out; std::copy_n(frame.begin(),6,out.dst.begin()); std::copy_n(frame.begin()+6,6,out.src.begin());
    out.code=frame[18]; out.id=frame[19];
    if (out.code==1 || out.code==2) {
        require(len>=5,"missing EAP type"); out.type=frame[22]; out.payload=slice(frame,23,len-5);
    } else out.payload=slice(frame,22,len-4);
    return out;
}
inline uint32_t checksum32(Bytes& v) {
    require(v.size()>=32,"short MiscInfo");
    const size_t n=size_t(v[2]) | (size_t(v[3])<<8);
    require(n>=32 && n<=v.size() && n%4==0,"invalid MiscInfo length");
    v[28]=126; uint32_t sum=0;
    for(size_t p=0;p<n;p+=4) sum^=le32(v,p);
    sum*=uint32_t(19680126); set_le32(v,24,sum); v[28]=0; return sum;
}
inline uint32_t checksum16(Bytes& v) {
    require(v.size()>=40,"short Heartbeat3"); uint32_t sum=0;
    for(size_t p=0;p<40;p+=2) sum^=uint32_t(v[p]) | uint32_t(v[p+1])<<8;
    sum*=711; set_le32(v,24,sum); return sum;
}
inline Bytes misc_alive() { return unhex("0700080001000000"); }
inline Bytes startup_heartbeat_windows31(const Bytes& random) {
    require(random.size()==2,"startup heartbeat random must be 2 bytes");
    Bytes v(40,0); const Bytes header{7,0,0x28,0,0x0b,1,0x0f,0x27};
    std::copy(header.begin(),header.end(),v.begin());
    std::copy(random.begin(),random.end(),v.begin()+8);
    return v;
}
inline Bytes misc_info(const Mac& mac, const IP& ip, const std::string& user,
                       const std::string& hostname, const IP& dns, const Bytes& flux) {
    require(flux.size()==4,"flux must be 4 bytes");
    Bytes v=unhex("0701f40003"); const size_t u=std::min(user.size(),size_t(25));
    v.push_back(uint8_t(u)); append(v,mac); append(v,ip); append(v,unhex("0222002a"));
    append(v,flux); append(v,unhex("c72f310100000000"));
    append(v,user.substr(0,u)); append(v,hostname.substr(0,44-u)); v.resize(76);
    append(v,dns); v.resize(96); // dns2 and reserved fields are zero in the original client.
    append(v,unhex("940000000600000002000000f023000002000000"));
    append(v,unhex("4472434f4d0096022a")); v.resize(180);
    append(v,std::string("4eb81fc048a5585b7dfe1783155241a328b103c6")); v.resize(244);
    checksum32(v); return v;
}
inline Bytes misc_info_windows31(const Mac& mac, const IP& ip, const std::string& user,
                                 const std::string& hostname, const IP& dns1, const IP& gateway,
                                 const IP& dns2, const Bytes& flux, const Bytes& trailer) {
    require(flux.size()==4,"flux must be 4 bytes");
    require(trailer.size()==16,"windows-31 trailer must be 16 bytes");
    Bytes v=unhex("0701040103"); const size_t u=std::min(user.size(),size_t(25));
    v.push_back(uint8_t(u)); append(v,mac); append(v,ip); append(v,unhex("02220031"));
    append(v,flux); append(v,unhex("c72f310100000000"));
    append(v,user.substr(0,u)); append(v,hostname.substr(0,44-u)); v.resize(76);
    append(v,dns1); append(v,gateway); append(v,dns2); v.resize(96);
    v.resize(116); // The current Windows packet reports zeroed OS fields.
    append(v,unhex("4472434f4d00960231")); v.resize(180);
    append(v,std::string("b1b54da1eb64ed14592830f7f895c373d507472a")); v.resize(244);
    append(v,trailer); require(v.size()==260,"invalid windows-31 MiscInfo size");
    checksum32(v); return v;
}
inline Bytes heartbeat(uint8_t type, uint8_t counter, const Bytes& random, const Bytes& flux, const IP& ip) {
    require((type==1 || type==3) && random.size()==2 && flux.size()==4,"invalid heartbeat fields");
    // Fixed-size allocation also avoids GCC 11's vector growth overread warning.
    Bytes v(40,0); const Bytes header{7,counter,0x28,0,0x0b,type,0xdc,2};
    std::copy(header.begin(),header.end(),v.begin());
    std::copy(random.begin(),random.end(),v.begin()+8);
    std::copy(flux.begin(),flux.end(),v.begin()+16);
    if(type==3) { std::copy(ip.begin(),ip.end(),v.begin()+28); checksum16(v); }
    return v;
}
inline Bytes decrypt_info(Bytes v) {
    require(v.size()==16,"response info must be 16 bytes");
    for(size_t i=0;i<v.size();++i) { unsigned n=unsigned(i&7); v[i]=uint8_t((unsigned(v[i])<<n)|(unsigned(v[i])>>(8-n))); }
    return v;
}
inline Bytes alive(const Bytes& digest,const Bytes& decrypted,uint16_t timestamp) {
    require(digest.size()==16 && decrypted.size()==16,"invalid Alive fields");
    Bytes v(38,0); v[0]=0xff;
    std::copy(digest.begin(),digest.end(),v.begin()+1);
    std::copy(decrypted.begin(),decrypted.end(),v.begin()+20);
    v[36]=uint8_t(timestamp); v[37]=uint8_t(timestamp>>8); return v;
}
} // namespace scut
