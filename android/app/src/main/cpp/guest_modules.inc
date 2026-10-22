// Game DLLs that run as lifted code next to Speed.exe. server.dll is EA's LAN
// game server: hosting a LAN race does LoadLibrary("server.dll") and calls its
// StartServer/StopServer/IsServerRunning exports.
//
// Lifted code bakes absolute addresses, so each DLL is lifted (run_lift.py
// --prefix) from a copy relocated to a fixed guest base (prepare_module.py) and
// mapped here at that same base: the original file from the game folder, with
// its base relocations applied for that base. The layout check below ties the
// file to the generated code.
struct GuestModuleSpec {const char* name;uint32_t base,preferredBase,imageSize;bool lifted;const char* folder="";};
// Bases shared with scripts/prepare_modules.py (layout table there). They sit
// between NFSC.exe (ends 0xC2C000) and the first heap arena (0x01300000);
// 0x01060000-0x01240000 is left to NFSCO's palmont.dll and its trampolines.
constexpr GuestModuleSpec guestModuleSpecs[]={
    // DirectX runtime (the player's local copy): NFSC.exe imports its effect
    // framework and math.
    {"d3dx9_30.dll",0x00d00000u,0x00400000u,0x260000u,true},
    // Carbon links the Visual C++ 7.1 runtime dynamically (game folder copies).
    {"msvcr71.dll",0x00f60000u,0x7c340000u,0x56000u,true},
    {"msvcp71.dll",0x00fc0000u,0x7c3a0000u,0x7b000u,true},
    // EA's LAN game server: hosting a LAN race does LoadLibrary("server.dll").
#ifdef NFS_SERVER_DLL
    {"server.dll",0x01270000u,0x10000000u,0x6f000u,true},
#else
    {"server.dll",0x01270000u,0x10000000u,0x6f000u,false},
#endif
    // NFSCO (nfsco.club, translated with its authors' permission): the mod
    // itself and its logger, at the bases the patch capture used
    // (tools/nfsco_capture.c, scripts/prepare_nfsco.py).
    {"palmont.dll",0x01060000u,0x10000000u,0x17d000u,true,"NFSCO\\bin\\"},
    {"logging.dll",0x01240000u,0x10000000u,0x28000u,true},
    // ThirteenAG's NFSCarbon.WidescreenFix (MIT; the player's scripts\ copy),
    // which the ASI loader runs next to NFSCO on PC: camera/FOV, HUD and
    // front-end placement at the phone's aspect ratio.
    {"nfscarbon.widescreenfix.asi",0x01300000u,0x10000000u,0x40000u,true,"scripts\\"},
};
// NFSCO's trampoline pool: palmont's executable allocations are served from
// here in order (k32VirtualAlloc), at the addresses the capture recorded and
// src/recomp/gen_pool was lifted from.
constexpr uint32_t nfscoPoolBase=0x01200000u,nfscoPoolSize=0x40000u;
uint32_t nfscoPoolNext=nfscoPoolBase;
bool guestExecutable(uint32_t address){
    if(address>=imageBase&&address-imageBase<0x5c1000u)return true;   // NFSC.exe headers and .text
    if(address>=nfscoPoolBase&&address<nfscoPoolNext)return true;
    for(auto& spec:guestModuleSpecs)if(address>=spec.base&&address-spec.base<spec.imageSize)return true;
    return false;
}
uint32_t nfscoPoolAllocate(uint32_t bytes){
    uint32_t pages=(bytes+0xfffu)&~0xfffu;
    if(!pages||nfscoPoolNext+pages>nfscoPoolBase+nfscoPoolSize)return 0;
    uint32_t block=nfscoPoolNext;nfscoPoolNext+=pages;std::memset(ptr(block,pages),0,pages);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","NFSCO executable allocation %08x (%u bytes)",block,bytes);
    return block;
}
// Static TLS (__declspec(thread)) of palmont.dll and the WidescreenFix: each
// module's template is copied into a block per guest thread, reached through
// the TEB+0x2C array at the module's slot.
struct StaticTls {uint32_t start=0,bytes=0,zeroFill=0;};
std::vector<StaticTls> staticTls;
constexpr uint32_t staticTlsSlots=16;
void attachStaticTlsSlot(uint32_t tib,uint32_t slot){
    uint32_t array=read32(tib+0x2c);
    if(!array){array=allocate(staticTlsSlots*4);std::memset(ptr(array,staticTlsSlots*4),0,staticTlsSlots*4);write32(tib+0x2c,array);}
    if(read32(array+slot*4))return;
    auto& tls=staticTls[slot];uint32_t total=tls.bytes+tls.zeroFill+16,block=allocate(total);
    std::memset(ptr(block,total),0,total);
    if(tls.bytes)std::memcpy(ptr(block,tls.bytes),ptr(tls.start,tls.bytes),tls.bytes);
    write32(array+slot*4,block);
}
// A minimal PEB shared by every guest thread (TEB+0x30): image base, process
// heap, Windows XP SP2 version and empty RTL_USER_PROCESS_PARAMETERS. NFSCO's
// CRT reads PEB->ProcessParameters->Flags to tell a packaged app.
uint32_t guestPeb(){
    static uint32_t peb=0;if(peb)return peb;
    peb=allocate(0x1000);std::memset(ptr(peb,0x1000),0,0x1000);uint32_t parameters=peb+0x800;
    write32(peb+0x8,imageBase);write32(peb+0x10,parameters);write32(peb+0x18,GuestHeaps::process);
    write32(peb+0xa4,5);write32(peb+0xa8,1);write32(peb+0xac,2600);write32(peb+0xb0,2);  // OSMajor/Minor/Build, Platform
    write32(parameters,0x298);write32(parameters+4,0x298);                              // MaximumLength, Length
    return peb;
}
void attachStaticTls(uint32_t tib){
    if(!read32(tib+0x30))write32(tib+0x30,guestPeb());
    for(uint32_t slot=0;slot<staticTls.size();++slot)attachStaticTlsSlot(tib,slot);
}
bool isLiftedModuleName(const std::string& dll){for(auto& s:guestModuleSpecs)if(dll==s.name)return s.lifted;return false;}
struct GuestModule {
    const GuestModuleSpec* spec;uint32_t entry=0,references=0;
    std::map<std::string,uint32_t> exports;  // name or "#ordinal" -> VA
};
std::map<uint32_t,GuestModule> guestModules;  // by base (the HMODULE)

GuestModule* findGuestModule(uint32_t handle){auto found=guestModules.find(handle);return found==guestModules.end()?nullptr:&found->second;}
GuestModule* findGuestModule(const std::string& name){
    for(auto& [base,module]:guestModules)if(module.spec->name==name)return &module;
    return nullptr;
}
bool isGuestModule(uint32_t handle){return findGuestModule(handle)!=nullptr;}
std::string guestModuleFilePath(uint32_t handle){
    if(!handle||handle==imageBase)return "C:\\nfsc\\NFSC.exe";
    auto* module=findGuestModule(handle);
    if(!module)return {};
    return std::string("C:\\nfsc\\")+module->spec->folder+module->spec->name;
}
uint32_t guestModuleContaining(uint32_t address){for(auto& [base,module]:guestModules)if(address>=base&&address-base<module.spec->imageSize)return base;return 0;}
uint32_t guestModuleHandle(const std::string& name){auto* module=findGuestModule(name);return module?module->spec->base:0;}
bool guestModuleExport(uint32_t handle,const std::string& name,uint32_t& va){
    auto* module=findGuestModule(handle);if(!module)return false;
    auto found=module->exports.find(name);if(found==module->exports.end())return false;
    va=found->second;return true;
}
void applyRelocations(uint32_t base,uint32_t size,uint32_t rva,uint32_t bytes,int64_t delta){
    moduleRange(size,rva,bytes);
    for(uint32_t offset=0;offset+8<=bytes;){
        uint32_t page=read32(base+rva+offset),blockSize=read32(base+rva+offset+4);
        if(blockSize<8||offset+blockSize>bytes)throw GuestStop("Bad base relocation block");
        for(uint32_t entry=8;entry+2<=blockSize;entry+=2){
            uint16_t value;std::memcpy(&value,ptr(base+rva+offset+entry,2),2);
            uint32_t type=value>>12,target=page+(value&0xfff);
            if(type==0)continue;  // IMAGE_REL_BASED_ABSOLUTE padding
            if(type!=3)throw GuestStop("Unsupported base relocation type "+std::to_string(type));
            moduleRange(size,target,4);write32(base+target,uint32_t(int64_t(read32(base+target))+delta));
        }
        offset+=blockSize;
    }
}
// Maps, relocates and binds the DLL, then runs DllMain(PROCESS_ATTACH). Returns the HMODULE or 0.
uint32_t loadGuestModule(const std::string& name){
    if(auto* loaded=findGuestModule(name)){++loaded->references;return loaded->spec->base;}
    const GuestModuleSpec* spec=nullptr;
    for(auto& candidate:guestModuleSpecs)if(name==candidate.name)spec=&candidate;
    if(!spec)return 0;
    if(!spec->lifted){NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSC","%s: this build has no lifted code for it",name.c_str());return 0;}
    std::ifstream stream(gamePath(std::string("C:\\nfsc\\")+spec->folder+name),std::ios::binary);
    if(!stream){NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSC","%s is missing from the game folder",name.c_str());return 0;}
    std::vector<unsigned char> file((std::istreambuf_iterator<char>(stream)),{});
    auto dos=fileStruct<pe_dos_header>(file,0);
    if(dos.e_magic!=0x5a4d||dos.e_lfanew<0)throw GuestStop(name+": invalid DOS header");
    auto nt=fileStruct<pe_nt_headers32>(file,dos.e_lfanew);auto opt=nt.OptionalHeader;
    if(nt.Signature!=0x4550||nt.FileHeader.Machine!=0x14c||opt.Magic!=0x10b||!(nt.FileHeader.Characteristics&0x2000))
        throw GuestStop(name+": expected an x86 PE32 DLL");
    uint32_t base=spec->base,size=opt.SizeOfImage,entry=base+opt.AddressOfEntryPoint;
    if(opt.ImageBase!=spec->preferredBase||size!=spec->imageSize||!recomp_lookup(entry))
        throw GuestStop(name+": file layout does not match the lifted code (base "+hex(opt.ImageBase)+", size "+hex(size)+")");
    memory->commit(base,size);
    std::memset(ptr(base,size),0,size);
    if(opt.SizeOfHeaders>file.size()||opt.SizeOfHeaders>size)throw GuestStop(name+": truncated PE headers");
    std::memcpy(ptr(base,opt.SizeOfHeaders),file.data(),opt.SizeOfHeaders);
    uint64_t sections=uint64_t(dos.e_lfanew)+24+nt.FileHeader.SizeOfOptionalHeader;
    for(uint32_t i=0;i<nt.FileHeader.NumberOfSections;++i){
        auto section=fileStruct<pe_section_header>(file,sections+i*sizeof(pe_section_header));
        moduleRange(size,section.VirtualAddress,std::max(section.VirtualSize,section.SizeOfRawData));
        if(uint64_t(section.PointerToRawData)+section.SizeOfRawData>file.size())throw GuestStop(name+": truncated PE section");
        std::memcpy(ptr(base+section.VirtualAddress,section.SizeOfRawData),file.data()+section.PointerToRawData,section.SizeOfRawData);
    }
    auto relocations=opt.DataDirectory[PE_DIR_BASERELOC];
    if(!relocations.Size)throw GuestStop(name+": no base relocations to move it to "+hex(base));
    applyRelocations(base,size,relocations.VirtualAddress,relocations.Size,int64_t(base)-int64_t(opt.ImageBase));
    auto tls=opt.DataDirectory[9];
    if(tls.Size){
        moduleRange(size,tls.VirtualAddress,24);uint32_t directory=base+tls.VirtualAddress;
        uint32_t start=read32(directory),end=read32(directory+4),index=read32(directory+8),callbacks=read32(directory+12);
        if(staticTls.size()>=staticTlsSlots)throw GuestStop(name+": too many modules with static TLS");
        if(callbacks&&read32(callbacks))throw GuestStop(name+": TLS callbacks are not provided");
        if(end<start||end-start>0x10000)throw GuestStop(name+": bad TLS template");
        uint32_t slot=uint32_t(staticTls.size());staticTls.push_back({start,end-start,read32(directory+16)});write32(index,slot);
        attachStaticTls(g_fs_base);
        for(auto& thread:guestThreads)if(thread->tib)attachStaticTls(thread->tib);
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","%s: static TLS %u bytes in slot %u",name.c_str(),end-start,slot);
    }
    size_t ownImports=deferredModuleImports.size();
    bindImports(base,size,opt.DataDirectory[PE_DIR_IMPORT].VirtualAddress,opt.DataDirectory[PE_DIR_IMPORT].Size);
    GuestModule module{spec,entry,1,{}};
    auto exports=opt.DataDirectory[0];
    if(exports.Size){
        moduleRange(size,exports.VirtualAddress,40);uint32_t directory=base+exports.VirtualAddress;
        uint32_t ordinalBase=read32(directory+16),functions=read32(directory+20),names=read32(directory+24);
        uint32_t addressTable=read32(directory+28),nameTable=read32(directory+32),ordinalTable=read32(directory+36);
        moduleRange(size,addressTable,uint64_t(functions)*4);moduleRange(size,nameTable,uint64_t(names)*4);moduleRange(size,ordinalTable,uint64_t(names)*2);
        for(uint32_t i=0;i<functions;++i)if(uint32_t rva=read32(base+addressTable+i*4))module.exports["#"+std::to_string(ordinalBase+i)]=base+rva;
        for(uint32_t i=0;i<names;++i){
            uint16_t index;std::memcpy(&index,ptr(base+ordinalTable+i*2,2),2);
            if(index<functions)module.exports[moduleString(base,size,read32(base+nameTable+i*4))]=base+read32(base+addressTable+index*4u);
        }
    }
    guestModules[base]=module;
    // Its own imports of lifted DLLs (msvcp71 -> msvcr71) before its DllMain.
    bindDeferredModuleImports(ownImports);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","%s mapped at %08x (preferred %08x), %zu exports; running DllMain",name.c_str(),base,opt.ImageBase,module.exports.size());
    if(!callGuest(entry,{base,1,0})){  // DLL_PROCESS_ATTACH
        guestModules.erase(base);
        NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSC","%s: DllMain refused PROCESS_ATTACH",name.c_str());
        return 0;
    }
    return base;
}
// FreeLibrary: the last reference runs DllMain(PROCESS_DETACH); the next load maps a fresh copy.
bool freeGuestModule(uint32_t handle){
    auto* module=findGuestModule(handle);if(!module)return false;
    if(--module->references)return true;
    uint32_t entry=module->entry;
    callGuest(entry,{handle,0,0});  // DLL_PROCESS_DETACH
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","%s unloaded",module->spec->name);
    guestModules.erase(handle);
    return true;
}

// Imports of lifted DLLs recorded by bindImports from index `first` on: map
// each DLL (its DllMain runs here, on the prepared guest machine) and store
// the export VAs. Loading a DLL appends its own imports, so walk by index.
void bindDeferredModuleImports(size_t first){
    size_t bound=0;
    for(size_t i=first;i<deferredModuleImports.size();++i){
        auto item=deferredModuleImports[i];
        uint32_t handle=loadGuestModule(item.dll),va=0;
        if(!handle)throw GuestStop(item.dll+" is required: copy it into the nfsc folder (game folder, or DirectX runtime from Windows/SysWOW64)");
        if(!guestModuleExport(handle,item.name,va))throw GuestStop(item.dll+" has no export "+item.name);
        write32(item.slot,va);++bound;
    }
    deferredModuleImports.resize(first);
    if(bound)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","Bound %zu imports to lifted DLLs",bound);
}

// NFSCO: the lifted NFSC.exe carries palmont's patches (jumps into palmont and
// its pool), so palmont must be loaded and attached before the game starts,
// as NFSCO's bootstrapper does on Windows.
// NFSCO ships its text only in English: every NFSCO/files/nfsco/LANGUAGES/
// <LANGUAGE>_*.BIN is a copy of ENGLISH_*.BIN with its new strings. For
// another language, rebuild those copies from NFSCO's English chunk with the
// game's own translation of each string NFSCO left unchanged (same hash and
// same English text as the game's English file); new and edited NFSCO
// strings stay in English. Chunk 0x39000: count, table offset, strings offset
// (from +8), name; table of (hash, string offset); 4-byte aligned; any
// chunks after it are kept.
struct LanguageChunk {std::vector<uint8_t> bytes;uint32_t count=0,table=0,strings=0,end=0;std::map<uint32_t,std::string> text;};
bool readLanguageChunk(const std::filesystem::path& path,LanguageChunk& chunk){
    std::ifstream in(path,std::ios::binary);if(!in)return false;
    chunk.bytes.assign(std::istreambuf_iterator<char>(in),{});auto& d=chunk.bytes;
    auto u32=[&](size_t at){uint32_t v=0;if(at+4<=d.size())std::memcpy(&v,d.data()+at,4);return v;};
    if(d.size()<0x24||u32(0)!=0x39000u)return false;
    chunk.end=8+u32(4);chunk.count=u32(8);chunk.table=8+u32(12);chunk.strings=8+u32(16);
    if(chunk.end>d.size()||chunk.table+uint64_t(chunk.count)*8>chunk.end||chunk.strings>chunk.end)return false;
    for(uint32_t i=0;i<chunk.count;++i){
        uint32_t hash=u32(chunk.table+i*8),at=chunk.strings+u32(chunk.table+i*8+4);if(at>=chunk.end)return false;
        auto* begin=reinterpret_cast<const char*>(d.data()+at);chunk.text[hash]=std::string(begin,strnlen(begin,chunk.end-at));
    }
    return true;
}
void applyNfscoTranslation(){
    static const std::map<std::string,std::pair<const char*,const char*>> files={  // registry name -> {game file, NFSCO file}
        {"Spanish",{"Spanish","SPANISH"}},{"Spanish (Mexican)",{"Mexican","MEXICAN"}},{"French",{"French","FRENCH"}},
        {"German",{"German","GERMAN"}},{"Italian",{"Italian","ITALIAN"}},{"Dutch",{"Dutch","DUTCH"}},{"Swedish",{"Swedish","SWEDISH"}},
        {"Danish",{"Danish","DANISH"}},{"Finnish",{"Finnish","FINNISH"}},{"Polish",{"Polish","POLISH"}},{"Czech",{"Czech","CZECH"}},
        {"Hungarian",{"Hungarian","HUNGARIAN"}},{"Korean",{"Korean","KOREAN"}},
        {"Chinese (Traditional)",{"Chinese_Trad","CHINESE_TRAD"}},{"Chinese (Simplified)",{"Chinese_Simp","CHINESE_SIMP"}}};
    auto found=files.find(guestLanguage);if(found==files.end())return;  // English: NFSCO's files as shipped
    unsigned translated=0,total=0;
    for(const char* part:{"FRONTEND","GLOBAL","INGAME"}){
        LanguageChunk nfsco,english,local;
        auto nfscoPath=gamePath(std::string("C:\\nfsc\\NFSCO\\files\\nfsco\\LANGUAGES\\ENGLISH_")+part+".BIN");
        if(!readLanguageChunk(nfscoPath,nfsco)||!readLanguageChunk(gamePath(std::string("C:\\nfsc\\LANGUAGES\\English_")+part+".bin"),english)
           ||!readLanguageChunk(gamePath(std::string("C:\\nfsc\\LANGUAGES\\")+found->second.first+"_"+part+".bin"),local)){
            NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSC","NFSCO translation: %s text files not readable; NFSCO text stays in English",part);continue;}
        std::vector<uint8_t> out(nfsco.bytes.begin(),nfsco.bytes.begin()+nfsco.strings);
        for(uint32_t i=0;i<nfsco.count;++i){
            uint32_t hash;std::memcpy(&hash,nfsco.bytes.data()+nfsco.table+i*8,4);
            std::string text=nfsco.text[hash];
            auto game=english.text.find(hash);auto translation=local.text.find(hash);
            if(game!=english.text.end()&&translation!=local.text.end()&&game->second==text){text=translation->second;++translated;}
            ++total;uint32_t offset=uint32_t(out.size()-nfsco.strings);std::memcpy(out.data()+nfsco.table+i*8+4,&offset,4);
            out.insert(out.end(),text.begin(),text.end());out.push_back(0);
        }
        while(out.size()%4)out.push_back(0);
        uint32_t size=uint32_t(out.size()-8);std::memcpy(out.data()+4,&size,4);
        out.insert(out.end(),nfsco.bytes.begin()+nfsco.end,nfsco.bytes.end());
        auto target=gamePath(std::string("C:\\nfsc\\NFSCO\\files\\nfsco\\LANGUAGES\\")+found->second.second+"_"+part+".BIN");
        std::ifstream current(target,std::ios::binary);std::vector<uint8_t> existing((std::istreambuf_iterator<char>(current)),{});current.close();
        if(existing==out)continue;
        std::ofstream file(target,std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char*>(out.data()),std::streamsize(out.size()));
    }
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","NFSCO text in %s: %u of %u strings translated from the game's own files",guestLanguage.c_str(),translated,total);
}
// NFSCO keeps the game language in NFSCO/registry.json ("language", the same
// names as the registry value) and applies it over the registry: write the
// launcher's choice there before palmont reads it.
void applyNfscoLanguage(){
    auto path=gamePath("C:\\nfsc\\NFSCO\\registry.json");
    std::ifstream in(path,std::ios::binary);if(!in)return;
    std::string text((std::istreambuf_iterator<char>(in)),{});in.close();
    size_t key=text.find("\"language\"");if(key==std::string::npos)return;
    size_t open=text.find('"',text.find(':',key));size_t close=open==std::string::npos?open:text.find('"',open+1);
    if(close==std::string::npos)return;
    if(text.compare(open+1,close-open-1,guestLanguage)==0)return;
    text.replace(open+1,close-open-1,guestLanguage);
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","NFSCO language set to %s",guestLanguage.c_str());
}
// WidescreenFix reads ResX/ResY from its .ini (0: the desktop size); give it
// the launcher's render size, creating the .ini with the PC defaults if the
// copy has none.
void writeWidescreenFixIni(){
    auto path=gamePath("C:\\nfsc\\scripts\\NFSCarbon.WidescreenFix.ini");
    std::string text;{std::ifstream in(path,std::ios::binary);if(in)text.assign(std::istreambuf_iterator<char>(in),{});}
    if(text.empty())text="[MAIN]\nResX = 0\nResY = 0\nFixHUD = 1\nFixFOV = 1\nScaling = 2\nHudWidescreenMode = 1\nFMVWidescreenMode = 1\n"
                         "[HUD]\nPiPPos1 = 0.203125\nPiPPos2 = 0.001953\nRearviewMirrorPos1 = -0.308594\nRearviewMirrorPos2 = 0.654053\n"
                         "[MISC]\nSkipIntro = 0\nWindowedMode = 0\nLightingFix = 1\nCarShadowFix = 1\nCustomUserFilesDirectoryInGameDir = 0\nExperimentalCrashFix = 1\n";
    auto set=[&](const char* key,uint32_t value){
        auto at=text.find(std::string("\n")+key);if(at==std::string::npos)return;
        auto end=text.find('\n',at+1);if(end==std::string::npos)end=text.size();
        text.replace(at+1,end-at-1,std::string(key)+" = "+std::to_string(value));
    };
    set("ResX",displayModeWidth());set("ResY",displayModeHeight());
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;
}
// NFSCO creates its window and the game device at NFSCO/window.json's size,
// saved from the first run: keep it equal to the launcher's render size so
// the resolution chosen there is the one used.
void writeNfscoWindow(){
    auto path=gamePath("C:\\nfsc\\NFSCO\\window.json");
    std::string text;{std::ifstream in(path,std::ios::binary);if(in)text.assign(std::istreambuf_iterator<char>(in),{});}
    if(text.empty())text="{\n\t\"height\": 0,\n\t\"mode\": \"borderless\",\n\t\"width\": 0\n}";
    auto set=[&](const char* key,uint32_t value){
        auto at=text.find(std::string("\"")+key+"\"");if(at==std::string::npos)return;
        auto colon=text.find(':',at);auto end=text.find_first_of(",}\n\r",colon);if(colon==std::string::npos||end==std::string::npos)return;
        text.replace(colon+1,end-colon-1," "+std::to_string(value));
    };
    set("width",displayModeWidth());set("height",displayModeHeight());
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;
}
// Give touch users enough time to read search results and room numbers.
// Retain longer user durations and slower fades, and preserve other settings.
void writeNfscoChat(){
    auto path=gamePath("C:\\nfsc\\NFSCO\\messenger.json");
    std::string text;{std::ifstream in(path,std::ios::binary);if(in)text.assign(std::istreambuf_iterator<char>(in),{});}
    if(text.empty())return; // Let NFSCO create its complete default configuration.
    bool changed=false;
    auto adjust=[&](const char* key,double limit,bool minimum){
        auto at=text.find(std::string("\"")+key+"\"");if(at==std::string::npos)return;
        auto colon=text.find(':',at);if(colon==std::string::npos)return;
        auto begin=text.find_first_not_of(" \t\r\n",colon+1);if(begin==std::string::npos)return;
        char* end=nullptr;double value=std::strtod(text.c_str()+begin,&end);
        if(end==text.c_str()+begin||!std::isfinite(value))return;
        if(minimum?value>=limit:(value>0&&value<=limit))return;
        text.replace(begin,size_t(end-(text.c_str()+begin)),std::to_string(limit));changed=true;
    };
    adjust("display_time",20.0,true);
    adjust("fade_speed",0.8,false);
    if(!changed)return;
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;
    if(!out)throw GuestStop("Could not save NFSCO chat display settings");
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","NFSCO chat: at least 20 seconds visible, slower fade");
}
// NFSCO reads these before installing its game settings hooks. Keep unrelated
// preferences (language, movies, audio and online settings) as the player saved them.
void writeNfscoGraphics(){
    if(!graphicsQuality)return;
    auto path=gamePath("C:\\nfsc\\NFSCO\\registry.json");
    std::string text;{std::ifstream in(path,std::ios::binary);if(in)text.assign(std::istreambuf_iterator<char>(in),{});}
    if(text.empty())text="{}";
    auto set=[&](const char* key,const std::string& value){
        auto at=text.find(std::string("\"")+key+"\"");
        if(at==std::string::npos){
            auto end=text.find_last_of('}');if(end==std::string::npos)throw GuestStop("Invalid NFSCO graphics settings file");
            auto open=text.find('{');if(open==std::string::npos)throw GuestStop("Invalid NFSCO graphics settings file");
            bool empty=text.find_first_not_of(" \t\r\n",open+1)==end;
            text.insert(end,(empty?"":",")+std::string("\n\t\"")+key+"\": "+value+"\n");return;
        }
        auto colon=text.find(':',at);auto end=text.find_first_of(",}\r\n",colon);
        if(colon==std::string::npos||end==std::string::npos)throw GuestStop("Invalid NFSCO graphics setting");
        text.replace(colon+1,end-colon-1," "+value);
    };
    const bool low=graphicsQuality==1,high=graphicsQuality==3;
    auto number=[&](const char* key,unsigned value){set(key,std::to_string(value));};
    auto boolean=[&](const char* key,bool value){set(key,value?"true":"false");};
    number("performance_level",5); // Custom: apply the individual values below.
    number("anti_aliasing",0);
    number("world_detail",low?0:high?3:1);
    number("vehicle_detail",low?0:1);
    number("shader_detail",low?0:high?3:1);
    number("shadow_detail",high?2:0);
    number("reflection_rate",high?3:0);
    number("road_reflection_detail",high?2:0);
    number("texture_filtering",low?0:high?2:1);
    boolean("are_particles_enabled",!low);
    boolean("are_weather_effects_enabled",high);
    boolean("is_post_processing_enabled",high);
    boolean("is_visual_treatment_enabled",!low);
    boolean("is_vertical_synchronization_enabled",false);
    auto temporary=path;temporary+=".tmp";
    {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out<<text;if(!out)throw GuestStop("Could not save NFSCO graphics settings");}
    std::filesystem::rename(temporary,path);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","NFSCO graphics profile %u (1 low, 2 medium, 3 high)",graphicsQuality);
}
void startNfsco(){
    memory->commit(nfscoPoolBase,nfscoPoolSize);
    applyNfscoLanguage();
    applyNfscoTranslation();
    writeWidescreenFixIni();
    writeNfscoWindow();
    writeNfscoChat();
    writeNfscoGraphics();
    if(!loadGuestModule("nfscarbon.widescreenfix.asi"))
        throw GuestStop("WidescreenFix is required: copy scripts/NFSCarbon.WidescreenFix.asi into the nfsc folder");
    attachStaticTls(g_fs_base);for(auto& thread:guestThreads)if(thread->tib)attachStaticTls(thread->tib);  // PEB
    uint32_t palmont=loadGuestModule("palmont.dll"),attach=0;
    if(!palmont)throw GuestStop("NFSCO is required: copy the NFSCO folder (with bin/palmont.dll) and logging.dll into the nfsc folder");
    if(!guestModuleExport(palmont,"OnAttach",attach))throw GuestStop("palmont.dll has no OnAttach export");
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","NFSCO: running palmont OnAttach at %08x",attach);
    callGuest(attach,{});
    if(nfscoPoolNext!=0x01221000u)
        throw GuestStop("NFSCO trampolines do not match the lifted pool: "+hex(nfscoPoolNext-nfscoPoolBase)+" bytes allocated, 0x21000 captured");
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSC","NFSCO attached: %u bytes of trampolines",nfscoPoolNext-nfscoPoolBase);
}
