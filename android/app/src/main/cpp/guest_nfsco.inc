// Win32 entries NFSCO's palmont.dll and logging.dll import (a recent MSVC CRT
// and a MinHook-style hooking library). stdcall: ret(value, argument count).

// Fiber local storage: the guest has no fibers, so FLS slots are TLS slots
// (destructor callbacks are not run at thread exit).
void k32FlsAlloc(){for(uint32_t i=0;i<tlsUsed.size();++i)if(!tlsUsed[i]){tlsUsed[i]=true;tlsValues[i]=0;ret(i,1);return;}lastError=8;ret(UINT32_MAX,1);}
void k32FlsFree(){uint32_t i=arg(0);bool ok=i<tlsUsed.size()&&tlsUsed[i];if(ok){tlsUsed[i]=false;tlsValues[i]=0;}else lastError=87;ret(ok,1);}
void k32FlsGetValue(){uint32_t i=arg(0);if(i>=tlsUsed.size()||!tlsUsed[i]){lastError=87;ret(0,1);}else{lastError=0;ret(tlsValues[i],1);}}
void k32FlsSetValue(){uint32_t i=arg(0);if(i>=tlsUsed.size()||!tlsUsed[i]){lastError=87;ret(0,2);}else{tlsValues[i]=arg(1);ret(1,2);}}
void k32InitializeCriticalSectionEx(){
    uint32_t address=arg(0);std::memset(ptr(address,24),0,24);write32(address+4,UINT32_MAX);
    criticalSections[address]=std::make_shared<std::recursive_mutex>();pendingCriticalDeletes.erase(address);ret(1,3);
}

// Slim reader/writer locks (zero-initialized, never deleted). Guest code runs
// one thread at a time under the machine lock, so the state is plain data and
// a waiter hands the machine over until the owner releases. Shared holders
// are counted like exclusive ones: correct, only less concurrent.
struct SrwState{uint32_t owner=0,count=0;};
std::map<uint32_t,SrwState> srwLocks;
void srwAcquire(uint32_t lock){
    auto& state=srwLocks[lock];
    if(state.count&&state.owner!=guestThreadId){
        WaitScope scope("AcquireSRWLock",lock);
        for(unsigned spins=0;srwLocks[lock].count&&srwLocks[lock].owner!=guestThreadId;++spins){checkGuestProgress();yieldGuestMachine(spins<64?0:1);}
    }
    auto& held=srwLocks[lock];held.owner=guestThreadId;++held.count;write32(lock,1);
}
void srwRelease(uint32_t lock){
    auto found=srwLocks.find(lock);
    if(found==srwLocks.end()||!found->second.count||found->second.owner!=guestThreadId)throw GuestStop("Unbalanced ReleaseSRWLock "+hex(lock)+" from "+hex(g_cur_func));
    if(!--found->second.count){srwLocks.erase(found);write32(lock,0);}
}
void k32AcquireSRWLock(){srwAcquire(arg(0));ret(0,1);}
void k32ReleaseSRWLock(){srwRelease(arg(0));ret(0,1);}
void k32TryAcquireSRWLockExclusive(){
    auto& state=srwLocks[arg(0)];
    if(state.count&&state.owner!=guestThreadId){ret(0,1);return;}
    state.owner=guestThreadId;++state.count;write32(arg(0),1);ret(1,1);
}
// Condition variables: a generation counter per CONDITION_VARIABLE address.
std::map<uint32_t,uint64_t> conditionGenerations;
void k32SleepConditionVariableSRW(){
    uint32_t condition=arg(0),lock=arg(1),milliseconds=arg(2);
    auto found=srwLocks.find(lock);
    if(found==srwLocks.end()||found->second.owner!=guestThreadId)throw GuestStop("SleepConditionVariableSRW without the lock");
    uint32_t depth=found->second.count;srwLocks.erase(found);write32(lock,0);
    uint64_t generation=conditionGenerations[condition];bool woken=false;
    {
        WaitScope scope("SleepConditionVariableSRW",condition);
        auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
        for(unsigned spins=0;;++spins){
            if(conditionGenerations[condition]!=generation){woken=true;break;}
            if(milliseconds!=UINT32_MAX&&std::chrono::steady_clock::now()>=deadline)break;
            checkGuestProgress();yieldGuestMachine(spins<64?0:1);
        }
    }
    srwAcquire(lock);srwLocks[lock].count=depth;
    if(!woken){lastError=1460;ret(0,4);return;}  // ERROR_TIMEOUT
    ret(1,4);
}
void k32WakeConditionVariable(){++conditionGenerations[arg(0)];ret(0,1);}  // waking all is allowed (spurious wakeups)

// Interlocked singly linked lists (SLIST_HEADER: first entry, depth/sequence).
void k32InitializeSListHead(){std::memset(ptr(arg(0),8),0,8);ret(0,1);}
void k32InterlockedFlushSList(){uint32_t head=arg(0),first=read32(head);std::memset(ptr(head,8),0,8);ret(first,1);}

void k32GetStartupInfoW(){std::memset(ptr(arg(0),68),0,68);write32(arg(0),68);ret(0,1);}
void k32GetSystemTimePreciseAsFileTime(){
    uint64_t ticks=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()/100)+116444736000000000ull;
    std::memcpy(ptr(arg(0),8),&ticks,8);ret(0,1);
}
void k32FlushInstructionCache(){ret(1,3);}  // lifted code does not follow written bytes
void k32GetLargePageMinimum(){ret(0,0);}
// MinHook freezes every other thread while it patches: report none.
void k32Thread32First(){lastError=18;ret(0,2);}  // ERROR_NO_MORE_FILES

// VirtualQuery: code of the lifted image, DLLs and the NFSCO pool is reported
// executable (MinHook checks a target before hooking it); other mapped guest
// memory is committed read/write, the rest free.
bool guestExecutable(uint32_t address);
// guest_modules.inc
uint32_t guestModuleContaining(uint32_t address);
uint32_t guestModuleHandle(const std::string& name);
void k32VirtualQuery(){
    uint32_t address=arg(0),out=arg(1),size=arg(2);
    if(size<28){lastError=24;ret(0,3);return;}
    uint32_t page=address&~0xfffu;std::memset(ptr(out,28),0,28);
    bool mapped=memory->readable(page,0x1000),code=guestExecutable(address);
    write32(out,page);write32(out+4,mapped?page:0);write32(out+8,code?0x40:mapped?0x04:0);write32(out+12,0x1000);
    write32(out+16,mapped?0x1000:0x10000);                                // MEM_COMMIT / MEM_FREE
    write32(out+20,code?0x40:mapped?0x04:0x01);                           // PAGE_EXECUTE_READWRITE / READWRITE / NOACCESS
    write32(out+24,code?0x1000000:mapped?0x20000:0);                       // MEM_IMAGE / MEM_PRIVATE
    ret(28,3);
}

// Wide-character file and module entries, on top of the ANSI ones: the guest
// paths are ASCII (the game folder); other characters become '?'.
std::string narrowGuest(uint32_t wide){
    std::string text;if(!wide)return text;
    for(uint32_t i=0;;++i){uint16_t c;std::memcpy(&c,ptr(wide+i*2,2),2);if(!c)break;text.push_back(c<128?char(c):'?');}
    return text;
}
void writeWide(uint32_t out,const std::string& text){
    for(size_t i=0;i<=text.size();++i){uint16_t c=i<text.size()?uint8_t(text[i]):0;std::memcpy(ptr(out+uint32_t(i)*2,2),&c,2);}
}
// Calls the ANSI entry with string argument `index` converted, then drops the
// extra stdcall arguments of the wide entry.
template<void(*Ansi)(),uint32_t ansiArgs,uint32_t wideArgs,uint32_t index> void wideCall(){
    uint32_t wide=index<wideArgs?arg(index):0,temporary=0;   // index >= wideArgs: no string argument
    if(wide){auto text=narrowGuest(wide);temporary=ansiString(text);write32(g_esp+4+index*4,temporary);}
    Ansi();g_esp+=(wideArgs-ansiArgs)*4;
    if(temporary)heaps->free(GuestHeaps::process,temporary);
}
void k32GetFileSizeEx(){
    auto file=fileHandles.find(arg(0));struct stat status{};
    if(file==fileHandles.end()){lastError=6;ret(0,2);return;}
    if(fstat(file->second,&status)){lastError=fileError();ret(0,2);return;}
    uint64_t size=uint64_t(status.st_size);write32(arg(1),uint32_t(size));write32(arg(1)+4,uint32_t(size>>32));ret(1,2);
}
void k32SetFilePointerEx(){
    auto file=fileHandles.find(arg(0));if(file==fileHandles.end()){lastError=6;ret(0,5);return;}
    uint32_t method=arg(4);if(method>2){lastError=87;ret(0,5);return;}
    int64_t distance=int64_t((uint64_t(arg(2))<<32)|arg(1));
    off_t result=::lseek(file->second,off_t(distance),method==0?SEEK_SET:method==1?SEEK_CUR:SEEK_END);
    if(result<0){lastError=errno==EINVAL?131:fileError();ret(0,5);return;}
    if(arg(3)){write32(arg(3),uint32_t(uint64_t(result)));write32(arg(3)+4,uint32_t(uint64_t(result)>>32));}
    ret(1,5);
}
// BY_HANDLE_FILE_INFORMATION (52 bytes) from fstat; the inode stands in for
// the file index, as std::filesystem compares files by it.
void k32GetFileInformationByHandle(){
    auto file=fileHandles.find(arg(0));uint32_t out=arg(1);struct stat status{};
    if(file==fileHandles.end()){lastError=6;ret(0,2);return;}
    if(fstat(file->second,&status)){lastError=fileError();ret(0,2);return;}
    std::memset(ptr(out,52),0,52);write32(out,S_ISDIR(status.st_mode)?0x10:0x20);
    auto time=[&](uint32_t offset,int64_t seconds){uint64_t ticks=uint64_t(seconds+11644473600ll)*10000000ull;std::memcpy(ptr(out+offset,8),&ticks,8);};
    time(4,status.st_ctime);time(12,status.st_atime);time(20,status.st_mtime);
    uint64_t size=S_ISDIR(status.st_mode)?0:uint64_t(status.st_size),index=uint64_t(status.st_ino);
    write32(out+28,uint32_t(status.st_dev));write32(out+32,uint32_t(size>>32));write32(out+36,uint32_t(size));
    write32(out+40,uint32_t(status.st_nlink));write32(out+44,uint32_t(index>>32));write32(out+48,uint32_t(index));
    ret(1,2);
}
// SYSTEMTIME (UTC) to local time in the phone's time zone (the CRT's stat()
// converts file times this way). A non-null TIME_ZONE_INFORMATION is refused.
void k32SystemTimeToTzSpecificLocalTime(){
    if(arg(0))throw GuestStop("SystemTimeToTzSpecificLocalTime with an explicit time zone is not provided");
    uint16_t in[8];std::memcpy(in,ptr(arg(1),16),16);
    std::tm utc{};utc.tm_year=in[0]-1900;utc.tm_mon=in[1]-1;utc.tm_mday=in[3];utc.tm_hour=in[4];utc.tm_min=in[5];utc.tm_sec=in[6];
    time_t seconds=timegm(&utc);std::tm local{};
    if(seconds==time_t(-1)||!localtime_r(&seconds,&local)){lastError=87;ret(0,3);return;}
    uint16_t out[8]={uint16_t(local.tm_year+1900),uint16_t(local.tm_mon+1),uint16_t(local.tm_wday),uint16_t(local.tm_mday),
                     uint16_t(local.tm_hour),uint16_t(local.tm_min),uint16_t(local.tm_sec),in[7]};
    std::memcpy(ptr(arg(2),16),out,16);ret(1,3);
}
// SHGetKnownFolderPath: the same profile folders as SHGetFolderPathA, inside
// the game folder; the string is CoTaskMemAlloc'd (freed by CoTaskMemFree).
void shellKnownFolderPath(){
    uint32_t id=read32(arg(0)),flags=arg(1),out=arg(3);std::string suffix;
    switch(id){  // GUID Data1
    case 0xfdd39ad0u:suffix="Documents";break;
    case 0xf1b32785u:suffix="AppData\\Local";break;
    case 0x3eb685dbu:suffix="AppData\\Roaming";break;
    case 0x33e28130u:suffix="Pictures";break;
    case 0x18989b1du:suffix="Videos";break;
    case 0x4c5c32ffu:suffix="Saved Games";break;
    default:throw GuestStop("SHGetKnownFolderPath folder "+hex(id)+" is not provided");
    }
    write32(out,0);
    std::string logical="C:\\nfsc\\"+suffix;auto path=gamePath(logical);std::error_code error;
    if(flags&0x8000)std::filesystem::create_directories(path,error);  // KF_FLAG_CREATE
    if(error){ret(0x80070005u,4);return;}
    if(!std::filesystem::is_directory(path,error)){ret(0x80070002u,4);return;}
    uint32_t text=heaps->alloc(GuestHeaps::process,0,uint32_t(logical.size()+1)*2);writeWide(text,logical);write32(out,text);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","Guest known folder %s",logical.c_str());ret(0,4);
}
// SHCreateDirectoryExA: every missing folder of the path, inside the game folder.
void shellCreateDirectoryExA(){
    auto path=gamePath(guestString(arg(1)));std::error_code error;
    if(std::filesystem::is_directory(path,error)){ret(183,3);return;}  // ERROR_ALREADY_EXISTS
    std::filesystem::create_directories(path,error);
    if(error){ret(error==std::errc::no_such_file_or_directory?3u:5u,3);return;}
    ret(0,3);
}
// WriteConsoleW (logging.dll): to logcat like WriteFile on stdout.
void k32WriteConsoleW(){
    uint32_t text=arg(1),count=arg(2);std::string line;
    for(uint32_t i=0;i<count;++i){uint16_t c;std::memcpy(&c,ptr(text+i*2,2),2);line.push_back(c<128?char(c):'?');}
    guestConsole(line);if(arg(3))write32(arg(3),count);ret(1,5);
}
void k32GetConsoleWindow(){ret(0,0);}
void oleCoTaskMemFree(){if(arg(0))heaps->free(GuestHeaps::process,arg(0));ret(0,1);}
// WIN32_FIND_DATAA (320 bytes) to WIN32_FIND_DATAW (592): same header, wide names.
void findDataToWide(uint32_t ansi,uint32_t wide){
    std::memcpy(ptr(wide,44),ptr(ansi,44),44);std::memset(ptr(wide+44,548),0,548);
    writeWide(wide+44,guestString(ansi+44).substr(0,259));writeWide(wide+564,guestString(ansi+304).substr(0,13));
}
void k32FindFirstFileExW(){
    uint32_t entry=g_esp,wideData=arg(2);if(arg(1)>1||arg(3)!=0){lastError=87;ret(UINT32_MAX,6);return;}  // FindExInfoStandard/Basic, FindExSearchNameMatch
    uint32_t name=ansiString(narrowGuest(arg(0))),data=allocate(320);
    write32(entry+4,name);write32(entry+8,data);
    findFirstFile();g_esp+=4*4;
    if(g_eax!=UINT32_MAX)findDataToWide(data,wideData);
    heaps->free(GuestHeaps::process,name);heaps->free(GuestHeaps::process,data);
}
void k32FindNextFileW(){
    uint32_t entry=g_esp,wideData=arg(1),data=allocate(320);write32(entry+8,data);
    findNextFile();if(g_eax)findDataToWide(data,wideData);
    heaps->free(GuestHeaps::process,data);
}
void k32GetCurrentDirectoryW(){
    const std::string path="C:\\nfsc";uint32_t capacity=arg(0),out=arg(1);
    if(capacity<=path.size()){ret(uint32_t(path.size()+1),2);return;}
    writeWide(out,path);ret(uint32_t(path.size()),2);
}
void k32GetFullPathNameW(){
    uint32_t entry=g_esp,capacity=arg(1),out=arg(2),part=arg(3);
    uint32_t name=ansiString(narrowGuest(arg(0))),buffer=allocate(capacity+1);
    write32(entry+4,name);write32(entry+12,buffer);write32(entry+16,part?buffer:0);
    fullPathName();
    if(g_eax&&g_eax<capacity){
        auto text=guestString(buffer);writeWide(out,text);
        if(part)write32(part,text.back()=='\\'?0:out+uint32_t(text.find_last_of('\\')+1)*2);
    }
    heaps->free(GuestHeaps::process,name);heaps->free(GuestHeaps::process,buffer);
}
void k32GetModuleHandleExW(){
    uint32_t flags=arg(0),name=arg(1),out=arg(2),handle=0;if(!out){lastError=87;ret(0,3);return;}
    if(flags&4){  // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        handle=name>=imageBase&&name-imageBase<imageSize?imageBase:guestModuleContaining(name);
    }else if(!name)handle=imageBase;
    else{
        auto text=lower(narrowGuest(name));if(text.find('.')==std::string::npos)text+=".dll";
        if(text=="nfsc.exe")handle=imageBase;else if(auto found=modules.find(text);found!=modules.end())handle=found->second;else handle=guestModuleHandle(text);
    }
    write32(out,handle);if(!handle){lastError=126;ret(0,3);return;}ret(1,3);
}

// LCMapStringEx: LCMapStringW with a locale name (user default, invariant or
// en-US, as the CRT's ctype setup passes) in place of the LCID.
void k32LCMapStringEx(){
    auto locale=arg(0)?narrowGuest(arg(0)):std::string();
    if(!locale.empty()&&locale!="en-US"&&locale!="!x-sys-default-locale")throw GuestStop("LCMapStringEx locale pending: "+locale);
    write32(g_esp+4,0x409);mapStringW();g_esp+=3*4;  // drop lpVersionInformation, lpReserved, sortHandle
}

// One display, as big as the render size the launcher chose: NFSCO builds its
// resolution and window setup from these, so it lands on the phone's aspect.
uint32_t displayModeWidth(){return renderWidth.load()?renderWidth.load():1280u;}
uint32_t displayModeHeight(){return renderHeight.load()?renderHeight.load():720u;}
constexpr uint32_t guestMonitor=0xf0080001u;
void userMonitorFromWindow(){ret(guestMonitor,2);}
void userGetMonitorInfoW(){
    uint32_t out=arg(1),size=read32(out);
    if(arg(0)!=guestMonitor||(size!=40&&size!=104)){lastError=87;ret(0,2);return;}
    std::memset(ptr(out+4,size-4),0,size-4);
    for(uint32_t rect:{4u,20u}){write32(out+rect+8,displayModeWidth());write32(out+rect+12,displayModeHeight());}  // rcMonitor, rcWork
    write32(out+36,1);  // MONITORINFOF_PRIMARY
    if(size==104)writeWide(out+40,"\\\\.\\DISPLAY1");
    ret(1,2);
}
// DEVMODEW: dmSize at 68, dmFields 72, dmBitsPerPel 168, dmPelsWidth 172,
// dmPelsHeight 176, dmDisplayFrequency 184. Mode 0 and the current mode
// (ENUM_CURRENT_SETTINGS) are the render size at 60 Hz.
void userEnumDisplaySettingsW(){
    uint32_t mode=arg(1),out=arg(2);
    if(mode!=0&&mode!=UINT32_MAX&&mode!=0xfffffffeu){lastError=18;ret(0,3);return;}
    std::memset(ptr(out,220),0,220);writeWide(out,"\\\\.\\DISPLAY1");
    write32(out+64,0x0401);write32(out+68,220);write32(out+72,0x5c0000u);  // dmSpecVersion, dmSize; BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY
    write32(out+168,32);write32(out+172,displayModeWidth());write32(out+176,displayModeHeight());write32(out+184,60);
    ret(1,3);
}
// DISPLAY_DEVICEW: DeviceName at 4, DeviceString at 68, StateFlags at 324.
void userEnumDisplayDevicesW(){
    uint32_t index=arg(1),out=arg(2);
    if(index||!out||read32(out)<840){lastError=18;ret(0,4);return;}
    std::memset(ptr(out+4,836),0,836);writeWide(out+4,"\\\\.\\DISPLAY1");writeWide(out+68,"Android Vulkan");
    write32(out+324,0x1|0x4);ret(1,4);  // ATTACHED_TO_DESKTOP | PRIMARY_DEVICE
}

// Named shared memory backed by the paging file (CreateFileMapping with
// INVALID_HANDLE_VALUE). NFSCO's bootstrapper hands palmont its command line
// in "command_line<pid>"; here no bootstrapper runs, so palmont creates the
// mapping itself, finds it new and uses its defaults. File-backed mappings
// are not provided. The handle is a kernel alias (CloseHandle accepts it).
struct SharedSection {uint32_t block,size;};
std::map<uint32_t,SharedSection> sharedSections;   // object id -> memory
std::map<std::string,uint32_t> sharedSectionNames;  // name -> object id
uint32_t nextSharedSection=0xf00a0001u;
void createSharedSection(const std::string& name){
    uint32_t file=arg(0),protect=arg(2)&0xff,high=arg(3),low=arg(4);
    if(file!=UINT32_MAX){
        // A read-only view of a file (NFSCO maps its .cso shaders): the bytes
        // are copied into guest memory; writes would not reach the file.
        if(protect!=0x02&&protect!=0x08)throw GuestStop("CreateFileMapping of a file for writing is not provided");
        auto handle=fileHandles.find(file);if(handle==fileHandles.end()){lastError=6;ret(0,6);return;}
        struct stat status{};if(fstat(handle->second,&status)){lastError=fileError();ret(0,6);return;}
        uint64_t size=high||low?(uint64_t(high)<<32|low):uint64_t(status.st_size);
        if(!size||size>uint64_t(status.st_size)||size>(256u<<20)){lastError=size?87:1006;ret(0,6);return;}  // ERROR_FILE_INVALID
        uint32_t block=allocate(uint32_t(size));
        for(uint64_t done=0;done<size;){ssize_t n=::pread(handle->second,ptr(block+uint32_t(done),uint32_t(size-done)),size_t(size-done),off_t(done));
            if(n<=0){heaps->free(GuestHeaps::process,block);lastError=n<0?fileError():38;ret(0,6);return;}done+=uint64_t(n);}
        uint32_t object=nextSharedSection++;sharedSections[object]={block,uint32_t(size)};
        uint32_t mapping=nextKernelHandle++;kernelHandleAliases.emplace(mapping,object);lastError=0;ret(mapping,6);return;
    }
    if(high||!low||low>(64u<<20)){lastError=87;ret(0,6);return;}
    uint32_t object=0;lastError=0;
    if(auto found=sharedSectionNames.find(name);!name.empty()&&found!=sharedSectionNames.end()){object=found->second;lastError=183;}  // ERROR_ALREADY_EXISTS
    else{
        object=nextSharedSection++;uint32_t block=allocate(low);std::memset(ptr(block,low),0,low);
        sharedSections[object]={block,low};if(!name.empty())sharedSectionNames[name]=object;
    }
    uint32_t handle=nextKernelHandle++;kernelHandleAliases.emplace(handle,object);
    uint32_t error=lastError;NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","Shared memory \"%s\" (%u bytes)%s",name.c_str(),low,error==183?" already exists":"");
    lastError=error;ret(handle,6);
}
void k32CreateFileMappingA(){createSharedSection(guestString(arg(5)));}
void k32CreateFileMappingW(){createSharedSection(narrowGuest(arg(5)));}
void k32MapViewOfFile(){
    auto alias=kernelHandleAliases.find(arg(0));auto section=alias==kernelHandleAliases.end()?sharedSections.end():sharedSections.find(alias->second);
    if(section==sharedSections.end()){lastError=6;ret(0,5);return;}
    uint32_t offset=arg(3),bytes=arg(4);
    if(arg(2)||offset>section->second.size||(bytes&&bytes>section->second.size-offset)){lastError=87;ret(0,5);return;}
    ret(section->second.block+offset,5);
}
void k32UnmapViewOfFile(){ret(1,1);}  // sections live as long as the guest

// palmont's window/overlay support (ImGui over the game window). The game
// window fills the Android surface at the origin, so client and screen
// coordinates are the same; there is no clipboard, IME or DWM frame.
void gdiGetStockObject(){ret(0xf00b0000u+(arg(0)&0xffu),1);}
void userIdentityPoint(){ret(1,2);}   // ClientToScreen / ScreenToClient
void userZero0(){ret(0,0);}           // GetCapture, GetMessageExtraInfo
void userIsWindowUnicode(){ret(0,1);}
void userTrackMouseEvent(){ret(1,1);}
// NFSCO centres its window on the monitor after every device reset; the
// size and WM_SIZE must follow, as GetClientRect/GetWindowRect report them.
void userMoveWindow(){
    uint32_t handle=arg(0);auto window=windows.find(handle);if(window==windows.end()){lastError=1400;ret(0,6);return;}
    int32_t x=arg(1),y=arg(2),width=arg(3),height=arg(4);
    static unsigned logged=0;
    if(logged++<16)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","MoveWindow %08x to %d,%d %dx%d (was %d,%d %dx%d)",handle,x,y,width,height,
        window->second.x,window->second.y,window->second.width,window->second.height);
    auto& value=window->second;bool moved=value.x!=x||value.y!=y,sized=value.width!=width||value.height!=height;
    value.x=x;value.y=y;value.width=width;value.height=height;
    if(moved)windowMessage(handle,3,0,(uint32_t(y)<<16)|(uint32_t(x)&0xffff));
    if(sized)windowMessage(handle,5,0,(uint32_t(height)<<16)|(uint32_t(width)&0xffff));
    ret(1,6);
}
void userOpenClipboard(){lastError=5;ret(0,1);}
void userClipboardNone0(){ret(0,0);}  // CloseClipboard, EmptyClipboard
void userClipboardNone1(){ret(0,1);}  // GetClipboardData
void userSetClipboardData(){ret(0,2);}
void immGetContext(){ret(0,1);}
void immTrue2(){ret(1,2);}            // ImmReleaseContext, ImmSetCompositionWindow, ImmSetCandidateWindow
void dwmSetWindowAttribute(){ret(0,4);}
void shellExecuteW(){
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","Guest ShellExecute %s %s (not opened)",narrowGuest(arg(1)).c_str(),narrowGuest(arg(2)).c_str());
    ret(2,6);  // <= 32: failure (ERROR_FILE_NOT_FOUND)
}
// Direct3D 9Ex is not bridged: callers fall back to Direct3DCreate9.
void d3dCreate9Ex(){
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","Direct3DCreate9Ex refused (D3DERR_NOTAVAILABLE) from %08x",g_cur_func);
    if(arg(1))write32(arg(1),0);ret(0x8876086au,2);
}

// WSASendTo (NFSCO's ENet-style client): the WSABUF list gathered into one
// datagram and sent by sendto. Overlapped sends are not provided.
void wsaSendTo(){
    uint32_t entry=g_esp,buffers=arg(1),count=arg(2),sentOut=arg(3),flags=arg(4),to=arg(5),toLength=arg(6);
    if(arg(7)||arg(8))throw GuestStop("Overlapped WSASendTo is not provided");
    uint32_t total=0;for(uint32_t i=0;i<count;++i)total+=read32(buffers+i*8);
    uint32_t data=allocate(total?total:1),at=0;
    for(uint32_t i=0;i<count;++i){uint32_t length=read32(buffers+i*8);if(length)std::memcpy(ptr(data+at,length),ptr(read32(buffers+i*8+4),length),length);at+=length;}
    write32(entry+8,data);write32(entry+12,total);write32(entry+16,flags);write32(entry+20,to);write32(entry+24,toLength);
    socketSendTo();g_esp+=3*4;  // sendto(s, buf, len, flags, to, tolen) has 3 arguments fewer
    heaps->free(GuestHeaps::process,data);
    if(g_eax==UINT32_MAX)return;  // SOCKET_ERROR, WSAGetLastError already set
    if(sentOut)write32(sentOut,g_eax);g_eax=0;
}

// Private profile (.ini) reads for ThirteenAG's WidescreenFix: [section]
// key = value, ';' or '//' comments, case-insensitive names.
std::string profileValue(const std::string& file,const std::string& section,const std::string& key,bool& found){
    found=false;std::ifstream in(gamePath(file));std::string line,current;
    auto trim=[](std::string s){size_t a=s.find_first_not_of(" \t\r"),b=s.find_last_not_of(" \t\r");return a==std::string::npos?std::string():s.substr(a,b-a+1);};
    while(std::getline(in,line)){
        for(const char* comment:{";","//"})if(auto at=line.find(comment);at!=std::string::npos)line.erase(at);
        line=trim(line);if(line.empty())continue;
        if(line.front()=='['){auto close=line.find(']');current=lower(trim(line.substr(1,close==std::string::npos?std::string::npos:close-1)));continue;}
        auto equals=line.find('=');if(equals==std::string::npos)continue;
        if(current==lower(section)&&lower(trim(line.substr(0,equals)))==lower(key)){found=true;return trim(line.substr(equals+1));}
    }
    return {};
}
void k32GetPrivateProfileIntA(){
    bool found;auto value=profileValue(guestString(arg(3)),guestString(arg(0)),guestString(arg(1)),found);
    ret(found?uint32_t(std::strtol(value.c_str(),nullptr,0)):arg(2),4);
}
void k32GetPrivateProfileStringA(){
    uint32_t out=arg(3),size=arg(4);if(!out||!size){ret(0,6);return;}
    if(!arg(0)||!arg(1))throw GuestStop("GetPrivateProfileStringA section/key enumeration is not provided");
    bool found;auto value=profileValue(guestString(arg(5)),guestString(arg(0)),guestString(arg(1)),found);
    if(!found)value=arg(2)?guestString(arg(2)):std::string();
    uint32_t count=std::min<uint32_t>(uint32_t(value.size()),size-1);
    std::memcpy(ptr(out,count),value.data(),count);*static_cast<char*>(ptr(out+count,1))=0;ret(count,6);
}
void userGetMonitorInfoA(){
    uint32_t out=arg(1),size=read32(out);
    if(arg(0)!=guestMonitor||(size!=40&&size!=72)){lastError=87;ret(0,2);return;}
    std::memset(ptr(out+4,size-4),0,size-4);
    for(uint32_t rect:{4u,20u}){write32(out+rect+8,displayModeWidth());write32(out+rect+12,displayModeHeight());}
    write32(out+36,1);
    if(size==72){static const char device[]="\\\\.\\DISPLAY1";std::memcpy(ptr(out+40,sizeof device),device,sizeof device);}
    ret(1,2);
}

const std::map<std::string,recomp_func_t> nfscoFunctions={
    {"GetPrivateProfileIntA",k32GetPrivateProfileIntA},{"GetPrivateProfileStringA",k32GetPrivateProfileStringA},
    {"GetMonitorInfoA",userGetMonitorInfoA},
    {"CreateEventW",wideCall<createEvent,4,4,3>},
    {"WSASendTo",wsaSendTo},
    {"GetStockObject",gdiGetStockObject},{"ClientToScreen",userIdentityPoint},{"ScreenToClient",userIdentityPoint},
    {"GetCapture",userZero0},{"GetMessageExtraInfo",userZero0},{"IsWindowUnicode",userIsWindowUnicode},
    {"TrackMouseEvent",userTrackMouseEvent},{"MoveWindow",userMoveWindow},
    {"PeekMessageW",peekMessage},{"DispatchMessageW",dispatchMessage},{"PostMessageW",postMessage},{"LoadCursorW",loadCursor},
    {"OpenClipboard",userOpenClipboard},{"CloseClipboard",userClipboardNone0},{"EmptyClipboard",userClipboardNone0},
    {"GetClipboardData",userClipboardNone1},{"SetClipboardData",userSetClipboardData},
    {"ImmGetContext",immGetContext},{"ImmReleaseContext",immTrue2},{"ImmSetCompositionWindow",immTrue2},{"ImmSetCandidateWindow",immTrue2},
    {"DwmSetWindowAttribute",dwmSetWindowAttribute},{"ShellExecuteW",shellExecuteW},{"Direct3DCreate9Ex",d3dCreate9Ex},
    {"LCMapStringEx",k32LCMapStringEx},
    {"CreateFileMappingA",k32CreateFileMappingA},{"CreateFileMappingW",k32CreateFileMappingW},
    {"MapViewOfFile",k32MapViewOfFile},{"UnmapViewOfFile",k32UnmapViewOfFile},
    {"MonitorFromWindow",userMonitorFromWindow},{"GetMonitorInfoW",userGetMonitorInfoW},
    {"EnumDisplaySettingsW",userEnumDisplaySettingsW},{"EnumDisplayDevicesW",userEnumDisplayDevicesW},
    {"CreateFileW",wideCall<createFile,7,7,0>},{"LoadLibraryExW",wideCall<loadLibrary,1,3,0>},
    {"WaitForSingleObjectEx",wideCall<waitThread,2,3,99>},
    {"GetFileSizeEx",k32GetFileSizeEx},{"SetFilePointerEx",k32SetFilePointerEx},{"GetFileInformationByHandle",k32GetFileInformationByHandle},
    {"SystemTimeToTzSpecificLocalTime",k32SystemTimeToTzSpecificLocalTime},
    {"SHGetKnownFolderPath",shellKnownFolderPath},{"CoTaskMemFree",oleCoTaskMemFree},{"WriteConsoleW",k32WriteConsoleW},{"GetConsoleWindow",k32GetConsoleWindow},{"SHCreateDirectoryExA",shellCreateDirectoryExA},
    {"FindFirstFileExW",k32FindFirstFileExW},{"FindNextFileW",k32FindNextFileW},
    {"GetCurrentDirectoryW",k32GetCurrentDirectoryW},{"GetFullPathNameW",k32GetFullPathNameW},
    {"GetModuleHandleExW",k32GetModuleHandleExW},
    {"FlsAlloc",k32FlsAlloc},{"FlsFree",k32FlsFree},{"FlsGetValue",k32FlsGetValue},{"FlsSetValue",k32FlsSetValue},
    {"InitializeCriticalSectionEx",k32InitializeCriticalSectionEx},
    {"AcquireSRWLockExclusive",k32AcquireSRWLock},{"AcquireSRWLockShared",k32AcquireSRWLock},
    {"ReleaseSRWLockExclusive",k32ReleaseSRWLock},{"ReleaseSRWLockShared",k32ReleaseSRWLock},
    {"TryAcquireSRWLockExclusive",k32TryAcquireSRWLockExclusive},
    {"SleepConditionVariableSRW",k32SleepConditionVariableSRW},{"WakeAllConditionVariable",k32WakeConditionVariable},
    {"WakeConditionVariable",k32WakeConditionVariable},
    {"InitializeSListHead",k32InitializeSListHead},{"InterlockedFlushSList",k32InterlockedFlushSList},
    {"GetStartupInfoW",k32GetStartupInfoW},{"GetSystemTimePreciseAsFileTime",k32GetSystemTimePreciseAsFileTime},
    {"FlushInstructionCache",k32FlushInstructionCache},{"GetLargePageMinimum",k32GetLargePageMinimum},
    {"Thread32First",k32Thread32First},{"Thread32Next",k32Thread32First},{"VirtualQuery",k32VirtualQuery},
};
