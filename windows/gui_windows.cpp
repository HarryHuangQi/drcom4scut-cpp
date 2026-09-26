#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr UINT WM_LOG=WM_APP+1;
constexpr UINT WM_FINISHED=WM_APP+2;
constexpr int ID_INTERFACE=1001,ID_REFRESH=1002,ID_NETWORK=1003,ID_SAVE=1004,
    ID_START=1005,ID_STOP=1006,ID_OPEN_CONFIG=1007,ID_ADVANCED=1008;

struct Adapter {
    std::wstring friendly;
    ULONG index=0,type=0;
    IF_OPER_STATUS status=IfOperStatusUnknown;
    std::string ipv4,mac;
};

HINSTANCE instance_handle{};
HWND main_window{},interface_combo{},actual_ip{},actual_mac{},log_box{},status_text{};
HWND auth_ip{},udp_ip{},gateway{},mac_field{},username{},password{},hostname{},server{},dns{};
HWND profile{},udp_local_port{},udp_trailer{},wake_time{},retry_count{},retry_interval{};
HWND reconnect_time{},eap_timeout{},udp_timeout{},heartbeat_interval{},run_seconds{};
HWND advanced_button{},log_label{};
std::vector<HWND> advanced_controls;
bool advanced_mode=false;
std::vector<Adapter> adapter_list;
HANDLE child_process=nullptr,child_thread=nullptr,stop_event=nullptr,log_read=nullptr;
std::wstring stop_event_name;

std::wstring wide(const std::string& value) {
    if(value.empty()) return {};
    const int count=MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),nullptr,0);
    std::wstring out(size_t(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),out.data(),count); return out;
}
std::string utf8(const std::wstring& value) {
    if(value.empty()) return {};
    const int count=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
    std::string out(size_t(count),'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),out.data(),count,nullptr,nullptr); return out;
}
std::wstring text(HWND control) {
    const int count=GetWindowTextLengthW(control);
    std::wstring value(size_t(count+1),L'\0');
    GetWindowTextW(control,value.data(),count+1); value.resize(size_t(count)); return value;
}
void set_text(HWND control,const std::wstring& value) { SetWindowTextW(control,value.c_str()); }

std::wstring app_data_dir() {
    wchar_t path[MAX_PATH]{};
    if(SHGetFolderPathW(nullptr,CSIDL_APPDATA|CSIDL_FLAG_CREATE,nullptr,SHGFP_TYPE_CURRENT,path)!=S_OK)
        throw std::runtime_error("Cannot locate AppData");
    std::wstring dir=std::wstring(path)+L"\\drcom4scut";
    CreateDirectoryW(dir.c_str(),nullptr); return dir;
}
std::wstring config_path() { return app_data_dir()+L"\\config.conf"; }
std::wstring exe_dir() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,DWORD(std::size(path)));
    std::wstring value(path); const auto pos=value.find_last_of(L"\\/");
    return pos==std::wstring::npos?L".":value.substr(0,pos);
}

std::vector<Adapter> enumerate_adapters() {
    ULONG size=16*1024; std::vector<unsigned char> storage(size);
    auto* first=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG result=GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_PREFIX,nullptr,first,&size);
    if(result==ERROR_BUFFER_OVERFLOW) {
        storage.resize(size); first=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result=GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_PREFIX,nullptr,first,&size);
    }
    if(result!=NO_ERROR) return {};
    std::vector<Adapter> out;
    for(auto* item=first;item;item=item->Next) {
        if(item->IfType!=IF_TYPE_ETHERNET_CSMACD) continue;
        Adapter a; a.friendly=item->FriendlyName?item->FriendlyName:L"";
        a.index=item->IfIndex; a.type=item->IfType; a.status=item->OperStatus;
        if(item->PhysicalAddressLength==6) {
            char value[18]{};
            std::snprintf(value,sizeof value,"%02x:%02x:%02x:%02x:%02x:%02x",
                item->PhysicalAddress[0],item->PhysicalAddress[1],item->PhysicalAddress[2],
                item->PhysicalAddress[3],item->PhysicalAddress[4],item->PhysicalAddress[5]);
            a.mac=value;
        }
        for(auto* addr=item->FirstUnicastAddress;addr;addr=addr->Next) {
            if(addr->Address.lpSockaddr && addr->Address.lpSockaddr->sa_family==AF_INET) {
                char value[INET_ADDRSTRLEN]{};
                const auto* v4=reinterpret_cast<const sockaddr_in*>(addr->Address.lpSockaddr);
                InetNtopA(AF_INET,&v4->sin_addr,value,sizeof value); a.ipv4=value; break;
            }
        }
        out.push_back(a);
    }
    return out;
}

void update_adapter_details() {
    const int selected=int(SendMessageW(interface_combo,CB_GETCURSEL,0,0));
    if(selected<0 || size_t(selected)>=adapter_list.size()) {
        set_text(actual_ip,L"-"); set_text(actual_mac,L"-"); return;
    }
    const auto& a=adapter_list[size_t(selected)];
    set_text(actual_ip,wide(a.ipv4.empty()?"-":a.ipv4)); set_text(actual_mac,wide(a.mac));
    if(text(mac_field).empty()) set_text(mac_field,wide(a.mac));
}
void refresh_adapters(const std::wstring& select=L"") {
    adapter_list=enumerate_adapters(); SendMessageW(interface_combo,CB_RESETCONTENT,0,0);
    int chosen=-1;
    for(size_t i=0;i<adapter_list.size();++i) {
        const auto& a=adapter_list[i];
        std::wstring label=a.friendly+L"  ["+wide(a.ipv4.empty()?"无 IPv4":a.ipv4)+L"]  "+
            (a.status==IfOperStatusUp?L"已连接":L"未连接");
        SendMessageW(interface_combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        if(a.friendly==select) chosen=int(i);
        if(chosen<0 && a.status==IfOperStatusUp && !a.ipv4.empty()) chosen=int(i);
    }
    if(chosen>=0) SendMessageW(interface_combo,CB_SETCURSEL,chosen,0);
    update_adapter_details();
}

std::map<std::string,std::string> load_values() {
    const std::wstring path=config_path();
    std::ifstream file(path.c_str(),std::ios::binary); std::map<std::string,std::string> values;
    std::string line;
    while(std::getline(file,line)) {
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty() || line[0]=='#') continue;
        const auto pos=line.find('='); if(pos!=std::string::npos) values[line.substr(0,pos)]=line.substr(pos+1);
    }
    return values;
}
void set_if_present(HWND field,const std::map<std::string,std::string>& values,const char* key,const wchar_t* fallback=L"") {
    auto found=values.find(key); set_text(field,found==values.end()?fallback:wide(found->second));
}
bool save_config(bool notify=true) {
    const int selected=int(SendMessageW(interface_combo,CB_GETCURSEL,0,0));
    if(selected<0 || size_t(selected)>=adapter_list.size()) {
        MessageBoxW(main_window,L"请先选择物理有线网卡。",L"配置不完整",MB_ICONWARNING); return false;
    }
    if(text(auth_ip).empty() || text(gateway).empty() || text(username).empty() || text(password).empty()) {
        MessageBoxW(main_window,L"认证 IPv4、实际网关、账号和密码不能为空。",L"配置不完整",MB_ICONWARNING); return false;
    }
    const std::wstring path=config_path();
    std::vector<std::string> lines;
    {
        std::ifstream existing(path.c_str(),std::ios::binary);
        std::string line;
        while(std::getline(existing,line)) {
            if(!line.empty() && line.back()=='\r') line.pop_back();
            lines.push_back(line);
        }
    }
    if(lines.empty()) {
        lines={
            "# Private GUI configuration. Do not publish this file.",
            "profile=windows-31",
            "udp-local-port=61440",
            "udp-trailer=2001025030007004aa0cb7dee93f3c65",
            "time=07:00",
            "retry=2",
            "interval=5000",
            "reconnect=15",
            "eap-timeout=60",
            "udp-timeout=12",
            "heartbeat-interval=12"
        };
    }
    std::map<std::string,std::optional<std::string>> updates;
    auto update=[&](const char* key,HWND field) { updates[key]=utf8(text(field)); };
    updates["interface"]=utf8(adapter_list[size_t(selected)].friendly);
    update("ip",auth_ip);
    updates["udp-ip"]=text(udp_ip).empty()?std::nullopt:std::optional<std::string>(utf8(text(udp_ip)));
    update("gateway",gateway);
    updates["mac"]=text(mac_field).empty()?std::nullopt:std::optional<std::string>(utf8(text(mac_field)));
    update("username",username); update("password",password); update("hostname",hostname);
    update("host",server); update("dns",dns);
    if(advanced_mode) {
        update("profile",profile); update("udp-local-port",udp_local_port);
        update("udp-trailer",udp_trailer); update("time",wake_time);
        update("retry",retry_count); update("interval",retry_interval);
        update("reconnect",reconnect_time); update("eap-timeout",eap_timeout);
        update("udp-timeout",udp_timeout); update("heartbeat-interval",heartbeat_interval);
        update("run-seconds",run_seconds);
    }

    std::vector<std::string> merged;
    std::set<std::string> written;
    for(const auto& line:lines) {
        const auto pos=line.find('=');
        const std::string key=pos==std::string::npos?std::string():line.substr(0,pos);
        const auto found=updates.find(key);
        if(found==updates.end()) { merged.push_back(line); continue; }
        if(!written.insert(key).second) continue;
        if(found->second) merged.push_back(key+'='+*found->second);
    }
    for(const auto& [key,value]:updates) {
        if(written.insert(key).second && value) merged.push_back(key+'='+*value);
    }

    std::ofstream file(path.c_str(),std::ios::binary|std::ios::trunc);
    if(!file) { MessageBoxW(main_window,L"无法写入配置文件。",L"保存失败",MB_ICONERROR); return false; }
    for(const auto& line:merged) file<<line<<'\n';
    file.close();
    if(notify) {
        const std::wstring detail=advanced_mode
            ? L"已保存精简和完整模式字段，并保留其他配置：\n"
            : L"仅保存精简模式字段；完整模式和其他配置保持不变：\n";
        MessageBoxW(main_window,(detail+config_path()).c_str(),L"保存成功",MB_ICONINFORMATION);
    }
    return true;
}

void append_log(const std::wstring& value) {
    const int length=GetWindowTextLengthW(log_box);
    SendMessageW(log_box,EM_SETSEL,length,length);
    SendMessageW(log_box,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(value.c_str()));
    SendMessageW(log_box,EM_SCROLLCARET,0,0);
}

void close_process_handles() {
    if(child_process) { CloseHandle(child_process); child_process=nullptr; }
    if(child_thread) { CloseHandle(child_thread); child_thread=nullptr; }
    if(log_read) { CloseHandle(log_read); log_read=nullptr; }
    if(stop_event) { CloseHandle(stop_event); stop_event=nullptr; }
}

void reader_thread(HANDLE pipe,HANDLE process) {
    std::string pending; char buffer[1024]; DWORD count=0;
    while(ReadFile(pipe,buffer,sizeof buffer,&count,nullptr) && count) {
        pending.append(buffer,buffer+count);
        auto* message=new std::wstring(wide(pending)); pending.clear();
        PostMessageW(main_window,WM_LOG,0,reinterpret_cast<LPARAM>(message));
    }
    WaitForSingleObject(process,INFINITE);
    DWORD code=0; GetExitCodeProcess(process,&code);
    PostMessageW(main_window,WM_FINISHED,code,0);
}

void start_client() {
    if(child_process) { MessageBoxW(main_window,L"认证客户端已经在运行。",L"提示",MB_ICONINFORMATION); return; }
    if(!save_config(false)) return;
    const std::wstring cli=exe_dir()+L"\\drcom4scut.exe";
    if(GetFileAttributesW(cli.c_str())==INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(main_window,(L"找不到：\n"+cli).c_str(),L"启动失败",MB_ICONERROR); return;
    }
    stop_event_name=L"Local\\drcom4scut-gui-"+std::to_wstring(GetCurrentProcessId());
    stop_event=CreateEventW(nullptr,TRUE,FALSE,stop_event_name.c_str());
    if(!stop_event) { MessageBoxW(main_window,L"无法创建停止事件。",L"启动失败",MB_ICONERROR); return; }
    SECURITY_ATTRIBUTES security{sizeof security,nullptr,TRUE}; HANDLE write_pipe=nullptr;
    if(!CreatePipe(&log_read,&write_pipe,&security,0)) { close_process_handles(); return; }
    SetHandleInformation(log_read,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW startup{}; startup.cb=sizeof startup; startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdOutput=write_pipe; startup.hStdError=write_pipe; startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::wstring command=L"\""+cli+L"\" --config \""+config_path()+L"\" --stop-event \""+stop_event_name+L"\"";
    std::vector<wchar_t> mutable_command(command.begin(),command.end()); mutable_command.push_back(L'\0');
    const BOOL created=CreateProcessW(cli.c_str(),mutable_command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW,nullptr,exe_dir().c_str(),&startup,&process);
    CloseHandle(write_pipe);
    if(!created) {
        MessageBoxW(main_window,(L"无法启动认证核心，错误码："+std::to_wstring(GetLastError())).c_str(),L"启动失败",MB_ICONERROR);
        close_process_handles(); return;
    }
    child_process=process.hProcess; child_thread=process.hThread;
    SetWindowTextW(status_text,L"状态：正在认证/保持在线");
    EnableWindow(GetDlgItem(main_window,ID_START),FALSE); EnableWindow(GetDlgItem(main_window,ID_STOP),TRUE);
    append_log(L"\r\n=== 启动认证 ===\r\n");
    std::thread(reader_thread,log_read,child_process).detach();
}

void stop_client() {
    if(stop_event) {
        SetEvent(stop_event); SetWindowTextW(status_text,L"状态：正在停止……");
        append_log(L"\r\n=== 请求正常停止 ===\r\n");
    }
}

HWND add_control(const wchar_t* type,const wchar_t* caption,DWORD style,int x,int y,int w,int h,int id=0,DWORD ex=0) {
    return CreateWindowExW(ex,type,caption,WS_CHILD|WS_VISIBLE|style,x,y,w,h,main_window,
        id?reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)):nullptr,instance_handle,nullptr);
}
HWND label(const wchar_t* value,int x,int y,int width=145) { return add_control(L"STATIC",value,0,x,y,width,22); }
HWND advanced_label(const wchar_t* value,int x,int y,int width=115) {
    HWND control=label(value,x,y,width); advanced_controls.push_back(control); return control;
}
HWND advanced_edit(const wchar_t* value,int x,int y,int width=125) {
    HWND control=add_control(L"EDIT",value,WS_BORDER|ES_AUTOHSCROLL,x,y,width,25);
    advanced_controls.push_back(control); return control;
}
void show_advanced(bool show) {
    advanced_mode=show;
    for(HWND control:advanced_controls) ShowWindow(control,show?SW_SHOW:SW_HIDE);
    SetWindowTextW(advanced_button,show?L"精简模式":L"完整模式");
    const int shift=show?150:0;
    SetWindowPos(log_label,nullptr,18,313+shift,145,22,SWP_NOZORDER);
    SetWindowPos(log_box,nullptr,18,338+shift,772,240,SWP_NOZORDER);
    SetWindowPos(main_window,nullptr,0,0,825,630+shift,SWP_NOMOVE|SWP_NOZORDER);
}

LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    switch(message) {
    case WM_CREATE: {
        main_window=window;
        HFONT font=reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        label(L"物理有线网卡",18,18); interface_combo=add_control(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_VSCROLL,165,14,430,260,ID_INTERFACE);
        add_control(L"BUTTON",L"刷新",0,605,14,70,27,ID_REFRESH);
        add_control(L"BUTTON",L"打开网卡设置",0,685,14,105,27,ID_NETWORK);
        label(L"网卡实际 IPv4",18,53); actual_ip=add_control(L"STATIC",L"-",SS_SUNKEN,165,50,230,24);
        label(L"网卡实际 MAC",410,53,120); actual_mac=add_control(L"STATIC",L"-",SS_SUNKEN,530,50,260,24);
        label(L"EAP 认证 IPv4 (ip=)",18,88); auth_ip=add_control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,165,84,230,25);
        label(L"UDP 载荷 IPv4",410,88,120); udp_ip=add_control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,530,84,260,25);
        label(L"实际有线网关",18,123); gateway=add_control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,165,119,230,25);
        label(L"认证 MAC",410,123,120); mac_field=add_control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,530,119,260,25);
        label(L"校园网账号",18,158); username=add_control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,165,154,230,25);
        label(L"校园网密码",410,158,120); password=add_control(L"EDIT",L"",WS_BORDER|ES_PASSWORD|ES_AUTOHSCROLL,530,154,260,25);
        label(L"主机名",18,193); hostname=add_control(L"EDIT",L"windows",WS_BORDER|ES_AUTOHSCROLL,165,189,230,25);
        label(L"UDP 服务器 (host=)",410,193,120); server=add_control(L"EDIT",L"s.scut.edu.cn",WS_BORDER|ES_AUTOHSCROLL,530,189,260,25);
        label(L"校园 DNS（逗号分隔）",18,228); dns=add_control(L"EDIT",L"202.38.193.33,222.201.130.30,202.112.17.33,222.201.130.33",WS_BORDER|ES_AUTOHSCROLL,165,224,625,25);
        add_control(L"BUTTON",L"保存配置",0,18,265,110,32,ID_SAVE);
        add_control(L"BUTTON",L"开始认证",0,138,265,110,32,ID_START);
        HWND stop=add_control(L"BUTTON",L"停止认证",0,258,265,110,32,ID_STOP); EnableWindow(stop,FALSE);
        add_control(L"BUTTON",L"打开配置文件",0,378,265,125,32,ID_OPEN_CONFIG);
        advanced_button=add_control(L"BUTTON",L"完整模式",0,510,265,90,32,ID_ADVANCED);
        status_text=add_control(L"STATIC",L"状态：未运行",SS_SUNKEN,610,269,180,25);

        advanced_label(L"协议 profile",18,313); profile=advanced_edit(L"windows-31",133,309);
        advanced_label(L"UDP 本地端口",278,313); udp_local_port=advanced_edit(L"61440",393,309);
        advanced_label(L"恢复时间",538,313); wake_time=advanced_edit(L"07:00",653,309,137);
        advanced_label(L"重试次数",18,348); retry_count=advanced_edit(L"2",133,344);
        advanced_label(L"重试间隔 ms",278,348); retry_interval=advanced_edit(L"5000",393,344);
        advanced_label(L"重连等待 s",538,348); reconnect_time=advanced_edit(L"15",653,344,137);
        advanced_label(L"EAP 超时 s",18,383); eap_timeout=advanced_edit(L"60",133,379);
        advanced_label(L"UDP 超时 s",278,383); udp_timeout=advanced_edit(L"12",393,379);
        advanced_label(L"保活间隔 s",538,383); heartbeat_interval=advanced_edit(L"12",653,379,137);
        advanced_label(L"运行时限 s",18,418); run_seconds=advanced_edit(L"0",133,414);
        advanced_label(L"UDP trailer",278,418,115); udp_trailer=advanced_edit(L"2001025030007004aa0cb7dee93f3c65",393,414,397);

        log_label=label(L"运行日志",18,313); log_box=add_control(L"EDIT",L"",WS_BORDER|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,18,338,772,240,0,WS_EX_CLIENTEDGE);
        EnumChildWindows(window,[](HWND child,LPARAM value)->BOOL{SendMessageW(child,WM_SETFONT,value,TRUE);return TRUE;},reinterpret_cast<LPARAM>(font));
        const auto values=load_values(); const auto found=values.find("interface");
        refresh_adapters(found==values.end()?L"":wide(found->second));
        set_if_present(auth_ip,values,"ip"); set_if_present(udp_ip,values,"udp-ip");
        set_if_present(gateway,values,"gateway"); set_if_present(mac_field,values,"mac");
        set_if_present(username,values,"username"); set_if_present(password,values,"password");
        set_if_present(hostname,values,"hostname",L"windows"); set_if_present(server,values,"host",L"s.scut.edu.cn");
        set_if_present(dns,values,"dns",L"202.38.193.33,222.201.130.30,202.112.17.33,222.201.130.33");
        set_if_present(profile,values,"profile",L"windows-31");
        set_if_present(udp_local_port,values,"udp-local-port",L"61440");
        set_if_present(udp_trailer,values,"udp-trailer",L"2001025030007004aa0cb7dee93f3c65");
        set_if_present(wake_time,values,"time",L"07:00");
        set_if_present(retry_count,values,"retry",L"2"); set_if_present(retry_interval,values,"interval",L"5000");
        set_if_present(reconnect_time,values,"reconnect",L"15"); set_if_present(eap_timeout,values,"eap-timeout",L"60");
        set_if_present(udp_timeout,values,"udp-timeout",L"12");
        set_if_present(heartbeat_interval,values,"heartbeat-interval",L"12");
        set_if_present(run_seconds,values,"run-seconds",L"0");
        // A missing mac= means "use the selected physical adapter's real MAC".
        update_adapter_details();
        show_advanced(false);
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wparam)==ID_INTERFACE && HIWORD(wparam)==CBN_SELCHANGE) update_adapter_details();
        else if(LOWORD(wparam)==ID_REFRESH) refresh_adapters();
        else if(LOWORD(wparam)==ID_NETWORK) ShellExecuteW(window,L"open",L"ms-settings:network-ethernet",nullptr,nullptr,SW_SHOWNORMAL);
        else if(LOWORD(wparam)==ID_SAVE) save_config();
        else if(LOWORD(wparam)==ID_START) start_client();
        else if(LOWORD(wparam)==ID_STOP) stop_client();
        else if(LOWORD(wparam)==ID_OPEN_CONFIG) ShellExecuteW(window,L"open",config_path().c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        else if(LOWORD(wparam)==ID_ADVANCED) show_advanced(!advanced_mode);
        return 0;
    case WM_LOG: {
        std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lparam)); append_log(*value); return 0;
    }
    case WM_FINISHED:
        SetWindowTextW(status_text,(L"状态：已停止，退出码 "+std::to_wstring(wparam)).c_str());
        EnableWindow(GetDlgItem(window,ID_START),TRUE); EnableWindow(GetDlgItem(window,ID_STOP),FALSE);
        close_process_handles(); return 0;
    case WM_CLOSE:
        if(stop_event) { SetEvent(stop_event); if(child_process && WaitForSingleObject(child_process,3000)==WAIT_TIMEOUT) TerminateProcess(child_process,2); }
        DestroyWindow(window); return 0;
    case WM_DESTROY: close_process_handles(); PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window,message,wparam,lparam);
    }
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    instance_handle=instance;
    WSADATA data{}; if(WSAStartup(MAKEWORD(2,2),&data)!=0) return 1;
    INITCOMMONCONTROLSEX controls{sizeof controls,ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSEXW cls{}; cls.cbSize=sizeof cls; cls.lpfnWndProc=window_proc; cls.hInstance=instance;
    cls.hCursor=LoadCursor(nullptr,IDC_ARROW); cls.hIcon=LoadIcon(nullptr,IDI_APPLICATION);
    cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1); cls.lpszClassName=L"Drcom4scutGui";
    RegisterClassExW(&cls);
    HWND window=CreateWindowExW(0,cls.lpszClassName,L"DrCOM4SCUT Windows 认证客户端",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,825,630,
        nullptr,nullptr,instance,nullptr);
    if(!window) return 1;
    ShowWindow(window,show); UpdateWindow(window);
    MSG message{}; while(GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
    WSACleanup(); return int(message.wParam);
}
