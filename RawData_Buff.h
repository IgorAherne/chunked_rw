// MIT LICENSE
// igor.aherne.business@gmail.com
// Requires C++17  for std::filesystem 

#pragma once
#include <memory>
#include <cassert>

// reader can unload bytes into here.
// You can then read items from it.
class RawData_Buff {

    inline void cleanup(){
        if(_data != nullptr){  _aligned_free(_data);  }
        _data = nullptr;
        _allocatedSize = _size = _currIx = 0;
    }

    RawData_Buff(const RawData_Buff& other) = delete;
    RawData_Buff& operator=(const RawData_Buff& other) = delete;

public:
    RawData_Buff(size_t sizeBytes){
        _data = (unsigned char*)_aligned_malloc(sizeBytes, 16);
        _allocatedSize = sizeBytes;
        _size = 0;//see 'set_apparent_size()'
        _currIx = 0;
    }
    ~RawData_Buff(){
        cleanup();
    }

public:
    unsigned char* data_begin(){ return _data; }
    unsigned char* data_current(){ return _data +_currIx; }

    void reset_ix() { _currIx = 0; }
    size_t size()const{ return _size; }
    size_t totalAlocatedSize()const{ return _allocatedSize; }
    size_t remaining()const{ return _size - _currIx; }
    void skipBytes(size_t numBytes){ _currIx+=numBytes; }
    bool endReached(){ return _currIx >= _size;  }


     void fill(const unsigned char* buff,  size_t numBytes ){
        //you can't write more bytes than what was allocated in our constructor:
         assert(numBytes <= _allocatedSize);
        _size = numBytes;
        std::memcpy(_data, buff, numBytes);
    }

    
    void set_apparent_size(size_t newSize){
        assert(newSize <= _allocatedSize);
        _size = newSize;
    }


private:
    unsigned char* _data = nullptr;

    size_t _size = 0;//less than or equal to '_allocatedSize' (in bytes)
    size_t _allocatedSize = 0;//(in bytes)

    size_t _currIx = 0;//how far we are into _data.
};
