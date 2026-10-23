struct ResourceLock {void* native;uint32_t guest,size,pitch,rowBytes,rows,flags;CompressedView compressed{};RECT rectangle{};uint32_t nativePitch{};bool mirrored=false;uint32_t offset=0,depth=1,extraNative=0;};
std::map<uint64_t,ResourceLock> resourceLocks;
uint64_t lockKey(uint32_t object,uint32_t subresource){return (uint64_t(object)<<32)|subresource;}
uint32_t rowBytes(D3DFORMAT format,uint32_t width){
    switch(format){
    case D3DFMT_DXT1:return ((width+3)/4)*8;
    case D3DFMT_DXT2:case D3DFMT_DXT3:case D3DFMT_DXT4:case D3DFMT_DXT5:return ((width+3)/4)*16;
    case D3DFMT_R8G8B8:return width*3;
    case D3DFMT_A8:case D3DFMT_L8:case D3DFMT_P8:return width;
    case D3DFMT_R5G6B5:case D3DFMT_X1R5G5B5:case D3DFMT_A1R5G5B5:case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:case D3DFMT_A8L8:case D3DFMT_L16:case D3DFMT_R16F:case D3DFMT_V8U8:return width*2;
    case D3DFMT_A8R8G8B8:case D3DFMT_X8R8G8B8:case D3DFMT_A8B8G8R8:case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10:case D3DFMT_A2B10G10R10:case D3DFMT_G16R16:case D3DFMT_R32F:
    case D3DFMT_G16R16F:case D3DFMT_Q8W8V8U8:case D3DFMT_V16U16:return width*4;
    case D3DFMT_A16B16G16R16:case D3DFMT_A16B16G16R16F:case D3DFMT_G32R32F:return width*8;
    case D3DFMT_A32B32G32R32F:return width*16;
    default:throw std::runtime_error("D3D9 lock pixel format pending: "+std::to_string(format));
    }
}
bool blockCompressed(D3DFORMAT format){return format>=D3DFMT_DXT1&&format<=D3DFMT_DXT5;}
HRESULT lockPixels(uint32_t address,uint32_t subresource,const D3DSURFACE_DESC& desc,
                   const D3DLOCKED_RECT& native,const RECT* rect,uint32_t flags,uint32_t output){
    uint32_t width=rect?rect->right-rect->left:desc.Width,height=rect?rect->bottom-rect->top:desc.Height;
    auto view=compressionView(address);
    if(view.texture){
        if(subresource){view.level=subresource%32;view.face=subresource/32;}
        auto& texture=*view.texture;uint32_t fullWidth=texture.mipWidth(view.level),fullHeight=texture.mipHeight(view.level);
        RECT region=rect?*rect:RECT{0,0,static_cast<LONG>(fullWidth),static_cast<LONG>(fullHeight)};
        if(region.left<0||region.top<0||region.right>static_cast<LONG>(fullWidth)||region.bottom>static_cast<LONG>(fullHeight)||region.right<=region.left||region.bottom<=region.top)
            throw std::runtime_error("Invalid compressed lock rectangle");
        if((region.left%4)||(region.top%4)||(region.right%4&&region.right!=static_cast<LONG>(fullWidth))||(region.bottom%4&&region.bottom!=static_cast<LONG>(fullHeight)))
            throw std::runtime_error("Compressed lock rectangle must follow block boundaries");
        if(native.Pitch<=0||static_cast<uint32_t>(native.Pitch)<static_cast<uint32_t>(region.right-region.left)*4)throw std::runtime_error("Invalid decoded texture pitch");
        uint32_t pitch=texture.pitch(view.level),rows=(region.bottom-region.top+3)/4,bytes=((region.right-region.left+3)/4)*texture.blockBytes();
        uint32_t temporary=allocateGuest(pitch*rows),offset=(region.top/4)*pitch+(region.left/4)*texture.blockBytes();auto& raw=texture.bytes(view.face,view.level);
        if(!(flags&D3DLOCK_DISCARD))for(uint32_t row=0;row<rows;++row)std::memcpy(static_cast<uint8_t*>(pointer(temporary))+row*pitch,raw.data()+offset+row*pitch,bytes);
        resourceLocks.emplace(lockKey(address,subresource),ResourceLock{native.pBits,temporary,pitch*rows,pitch,bytes,rows,flags,view,region,static_cast<uint32_t>(native.Pitch)});
        write(output,pitch);write(output+4,temporary);return D3D_OK;
    }
    uint32_t rows=blockCompressed(desc.Format)?(height+3)/4:height,bytes=rowBytes(desc.Format,width);
    if(native.Pitch<=0||static_cast<uint32_t>(native.Pitch)<bytes||!rows)throw std::runtime_error("Invalid D3D9 lock pitch");
    uint64_t size=uint64_t(native.Pitch)*rows;if(size>0x7fffffff)throw std::runtime_error("D3D9 lock allocation too large");
    uint32_t temporary=allocateGuest(size);
    if(!(flags&D3DLOCK_DISCARD))for(uint32_t row=0;row<rows;++row)std::memcpy(static_cast<uint8_t*>(pointer(temporary))+row*native.Pitch,static_cast<const uint8_t*>(native.pBits)+row*native.Pitch,bytes);
    resourceLocks.emplace(lockKey(address,subresource),ResourceLock{native.pBits,temporary,static_cast<uint32_t>(size),static_cast<uint32_t>(native.Pitch),bytes,rows,flags});
    write(output,native.Pitch);write(output+4,temporary);return D3D_OK;
}
void copyLockBack(uint32_t address,uint32_t subresource){
    auto lock=resourceLocks.find(lockKey(address,subresource));if(lock==resourceLocks.end())throw std::runtime_error("D3D9 unlock without guest lock");
    auto& data=lock->second;if(!(data.flags&D3DLOCK_READONLY))for(uint32_t row=0;row<data.rows;++row)
        if(!data.compressed.texture)std::memcpy(static_cast<uint8_t*>(data.native)+row*data.pitch,static_cast<const uint8_t*>(pointer(data.guest))+row*data.pitch,data.rowBytes);
    if(data.compressed.texture&&!(data.flags&D3DLOCK_READONLY)){
        auto& texture=*data.compressed.texture;auto& raw=texture.bytes(data.compressed.face,data.compressed.level);
        uint32_t offset=(data.rectangle.top/4)*data.pitch+(data.rectangle.left/4)*texture.blockBytes();
        for(uint32_t row=0;row<data.rows;++row)std::memcpy(raw.data()+offset+row*data.pitch,static_cast<const uint8_t*>(pointer(data.guest))+row*data.pitch,data.rowBytes);
        uint32_t width=texture.mipWidth(data.compressed.level),height=texture.mipHeight(data.compressed.level);
        decodeDXTRect(texture.mode(),width,height,raw.data(),raw.size(),uint32_t(data.rectangle.left),uint32_t(data.rectangle.top),
                      uint32_t(data.rectangle.right),uint32_t(data.rectangle.bottom),static_cast<uint8_t*>(data.native),data.nativePitch);
    }
}
void releaseLock(uint32_t address,uint32_t subresource){auto lock=resourceLocks.find(lockKey(address,subresource));if(lock!=resourceLocks.end()){if(!lock->second.mirrored)freeGuest(lock->second.guest-lock->second.offset);resourceLocks.erase(lock);}}
HRESULT lockTexture(uint32_t address,uint32_t level,uint32_t output,const RECT* rect,uint32_t flags){
    if(!output||resourceLocks.count(lockKey(address,level)))return D3DERR_INVALIDCALL;
    auto texture=nativeObject<IDirect3DTexture9>(address);D3DSURFACE_DESC desc{};HRESULT status=texture->GetLevelDesc(level,&desc);if(FAILED(status))return status;
    D3DLOCKED_RECT lock{};status=texture->LockRect(level,&lock,rect,flags);if(FAILED(status))return status;
    return lockPixels(address,level,desc,lock,rect,flags,output);
}
HRESULT lockCube(uint32_t address,uint32_t face,uint32_t level,uint32_t output,const RECT* rect,uint32_t flags){
    uint32_t subresource=face*32+level;if(!output||resourceLocks.count(lockKey(address,subresource)))return D3DERR_INVALIDCALL;
    auto texture=nativeObject<IDirect3DCubeTexture9>(address);D3DSURFACE_DESC desc{};HRESULT status=texture->GetLevelDesc(level,&desc);if(FAILED(status))return status;
    D3DLOCKED_RECT lock{};status=texture->LockRect(static_cast<D3DCUBEMAP_FACES>(face),level,&lock,rect,flags);if(FAILED(status))return status;
    return lockPixels(address,subresource,desc,lock,rect,flags,output);
}
// Volume locks (D3DLOCKED_BOX {RowPitch, SlicePitch, pBits}): the guest copy
// keeps the native row pitch and packs slices back to back, so unlocking
// copies row by row through each native slice.
struct VolumeLock {void* native;uint32_t guest,rowPitch,slicePitch,rowBytes,rows,depth,flags;};
std::map<uint64_t,VolumeLock> volumeLocks;
HRESULT lockBoxPixels(uint32_t address,uint32_t subresource,const D3DVOLUME_DESC& desc,const D3DLOCKED_BOX& native,const D3DBOX* box,uint32_t flags,uint32_t output){
    if(blockCompressed(desc.Format))throw std::runtime_error("D3D9 compressed volume lock pending: "+std::to_string(desc.Format));
    uint32_t width=box?box->Right-box->Left:desc.Width,height=box?box->Bottom-box->Top:desc.Height,depth=box?box->Back-box->Front:desc.Depth;
    uint32_t bytes=rowBytes(desc.Format,width);
    if(native.RowPitch<=0||uint32_t(native.RowPitch)<bytes||native.SlicePitch<native.RowPitch*int(height)||!height||!depth)throw std::runtime_error("Invalid D3D9 volume lock pitch");
    uint64_t slice=uint64_t(native.RowPitch)*height,size=slice*depth;if(size>0x7fffffff)throw std::runtime_error("D3D9 volume lock allocation too large");
    uint32_t temporary=allocateGuest(size);
    for(uint32_t z=0;z<depth;++z)for(uint32_t row=0;row<height;++row)
        if(!(flags&D3DLOCK_DISCARD))std::memcpy(static_cast<uint8_t*>(pointer(temporary))+z*slice+row*native.RowPitch,static_cast<const uint8_t*>(native.pBits)+uint64_t(z)*native.SlicePitch+row*native.RowPitch,bytes);
    volumeLocks.emplace(lockKey(address,subresource),VolumeLock{native.pBits,temporary,uint32_t(native.RowPitch),uint32_t(native.SlicePitch),bytes,height,depth,flags});
    write(output,uint32_t(native.RowPitch));write(output+4,uint32_t(slice));write(output+8,temporary);return D3D_OK;
}
void unlockBoxPixels(uint32_t address,uint32_t subresource){
    auto lock=volumeLocks.find(lockKey(address,subresource));if(lock==volumeLocks.end())throw std::runtime_error("D3D9 volume unlock without guest lock");
    auto& data=lock->second;uint64_t slice=uint64_t(data.rowPitch)*data.rows;
    if(!(data.flags&D3DLOCK_READONLY))for(uint32_t z=0;z<data.depth;++z)for(uint32_t row=0;row<data.rows;++row)
        std::memcpy(static_cast<uint8_t*>(data.native)+uint64_t(z)*data.slicePitch+row*data.rowPitch,static_cast<const uint8_t*>(pointer(data.guest))+z*slice+row*data.rowPitch,data.rowBytes);
    freeGuest(data.guest);volumeLocks.erase(lock);
}
HRESULT lockVolumeTexture(uint32_t address,uint32_t level,uint32_t output,const D3DBOX* box,uint32_t flags){
    if(!output||volumeLocks.count(lockKey(address,level)))return D3DERR_INVALIDCALL;
    auto texture=nativeObject<IDirect3DVolumeTexture9>(address);D3DVOLUME_DESC desc{};HRESULT status=texture->GetLevelDesc(level,&desc);if(FAILED(status))return status;
    D3DLOCKED_BOX lock{};status=texture->LockBox(level,&lock,box,flags);if(FAILED(status))return status;
    return lockBoxPixels(address,level,desc,lock,box,flags,output);
}
HRESULT lockVolume(uint32_t address,uint32_t output,const D3DBOX* box,uint32_t flags){
    if(!output||volumeLocks.count(lockKey(address,0)))return D3DERR_INVALIDCALL;
    auto volume=nativeObject<IDirect3DVolume9>(address);D3DVOLUME_DESC desc{};HRESULT status=volume->GetDesc(&desc);if(FAILED(status))return status;
    D3DLOCKED_BOX lock{};status=volume->LockBox(&lock,box,flags);if(FAILED(status))return status;
    return lockBoxPixels(address,0,desc,lock,box,flags,output);
}
HRESULT lockSurface(uint32_t address,uint32_t output,const RECT* rect,uint32_t flags){
    if(!output||resourceLocks.count(lockKey(address,0)))return D3DERR_INVALIDCALL;
    auto surface=nativeObject<IDirect3DSurface9>(address);D3DSURFACE_DESC desc{};HRESULT status=surface->GetDesc(&desc);if(FAILED(status))return status;
    D3DLOCKED_RECT lock{};status=surface->LockRect(&lock,rect,flags);if(FAILED(status))return status;return lockPixels(address,0,desc,lock,rect,flags,output);
}
template<class Buffer,class Desc>HRESULT lockBuffer(uint32_t address,uint32_t offset,uint32_t size,uint32_t output,uint32_t flags){
    if(!output)return D3DERR_INVALIDCALL;
    auto buffer=nativeObject<Buffer>(address);Desc desc{};HRESULT status=buffer->GetDesc(&desc);if(FAILED(status))return status;
    if(offset>desc.Size||(size&&size>desc.Size-offset))return D3DERR_INVALIDCALL;
    // A buffer may be locked again before it is unlocked (D3D9 counts the
    // locks; Carbon reads vertices back that way while loading a race): hand
    // out the open copy, offset to the range asked for; only the last Unlock
    // copies back.
    if(auto open=resourceLocks.find(lockKey(address,0));open!=resourceLocks.end()){
        auto& lock=open->second;uint32_t end=size?offset+size:desc.Size;
        auto& entry=objects.at(address);
        if(lock.mirrored&&entry.mirror){++lock.depth;write(output,entry.mirror+offset);return D3D_OK;}
        // Outside the open range (Carbon's race loader at 0x72AF50): the guest
        // copy spans the whole buffer, so widen in place and keep the pointer
        // the outer lock handed out. A second native lock marks the wider
        // range for upload; each one is unlocked at the last Unlock.
        if(offset<lock.offset||end>lock.offset+lock.size){
            uint32_t low=std::min(offset,lock.offset),high=std::max(end,lock.offset+lock.size);
            void* native=nullptr;HRESULT widened=buffer->Lock(low,high-low,&native,0);if(FAILED(widened))return widened;
            auto base=static_cast<uint8_t*>(native)-low;auto copy=static_cast<uint8_t*>(pointer(lock.guest-lock.offset));
            if(low<lock.offset)std::memcpy(copy+low,base+low,lock.offset-low);
            if(high>lock.offset+lock.size)std::memcpy(copy+lock.offset+lock.size,base+lock.offset+lock.size,high-(lock.offset+lock.size));
            lock.guest-=lock.offset-low;lock.native=base+low;lock.offset=low;lock.size=lock.pitch=lock.rowBytes=high-low;++lock.extraNative;
        }
        ++lock.depth;write(output,lock.guest+(offset-lock.offset));return D3D_OK;
    }
    // Dynamic write-only buffers (particles, HUD, skids: locked many times a
    // frame) keep one guest copy for their lifetime. Every write reaches the
    // native buffer through these locks, so the copy already holds what the
    // native one does: no allocation and, above all, no read back from the
    // mapped GPU memory (uncached on Android) before the game writes.
    constexpr DWORD streaming=D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY;
    auto& entry=objects.at(address);
    if((desc.Usage&streaming)==streaming&&!entry.mirror){
        entry.mirror=allocateGuest(desc.Size);std::memset(pointer(entry.mirror),0,desc.Size);
    }
    void* native=nullptr;status=buffer->Lock(offset,size,&native,flags);if(FAILED(status))return status;
    if(!size)size=desc.Size-offset;
    if(entry.mirror){
        resourceLocks.emplace(lockKey(address,0),ResourceLock{native,entry.mirror+offset,size,size,size,1,flags,{},{},0,true,offset});
        write(output,entry.mirror+offset);return D3D_OK;
    }
    // Sized for the whole buffer so a nested lock can widen the range in place.
    uint32_t temporary=allocateGuest(desc.Size)+offset;
    if(!(flags&D3DLOCK_DISCARD))std::memcpy(pointer(temporary),native,size);
    resourceLocks.emplace(lockKey(address,0),ResourceLock{native,temporary,size,size,size,1,flags,{},{},0,false,offset});write(output,temporary);return D3D_OK;
}
