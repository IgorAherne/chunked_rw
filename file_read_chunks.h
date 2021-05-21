// MIT LICENSE
// igor.aherne.business@gmail.com
// Requires C++17  for std::filesystem 

#pragma once
#include <vector>
#include <fstream>
#include <string>
#include <filesystem>
#include <functional>
#include <thread>
#include "RawData_Buff.h"
#include <cassert>


//alias for a function pointer. 
typedef std::function<void(RawData_Buff& buff, bool isLastChunk)>  L_chunkFunc;


// Opens file and reads it by chunks.
// You process one chunk while more file is seamlessly loaded into the other chunk. 
// Then they swap. (chunks are only used internally, end user doesn't interact with them).
//
// The class allows you to get raw-byte-data, literals, strings, which will be provided
// from the current chunk. 
//
// The class takes care of cases where requested data is on the border of two chunks.
// When that happens, the class uses a special, third buffer to store the 'remainder' 
// from the recent buffer. It then combines the remainder with more data, once it arrives, 
// to provide the user with the requested data.
//
// See BeginRead()  
// See HasMoreForRead()    <-- for example, could be used when in a loop
// See EndRead()
//
// See read_rawData()      <-- for example, could be used when in a loop
// See read_Literal()    <-- int, float, struct (shallow, no deep copies), etc.
// See read_String()    <--ascii text, for example "hello, I am Igor"

class file_read_chunks{

public:
    file_read_chunks(size_t chunkBuffSize = 1024*1024 )
        :_buff_a(chunkBuffSize),
         _buff_b(chunkBuffSize){
    }

    ~file_read_chunks(){
        if(_file.is_open()){ _file.close(); }
    }

public:
    // fileName_with_exten:  for example,  myFile.someExtension
    void BeginRead(const std::string& fileName_with_exten){
        EndRead();//just in case
        
        std::filesystem::path p(fileName_with_exten);
        _file.open(p, std::ios::binary);

        if (_file.is_open() == false){
            std::string message = std::string("file_read_chunks() could not open filePath: ") + fileName_with_exten;
            throw std::runtime_error(message);
            return;
        }
        
        _chunkSize =     _buff_a.totalAlocatedSize();
        _fileByteSize =  std::filesystem::file_size(p);//throws exception if path doesn't exist. 
        _numChunks =     _fileByteSize / _chunkSize;
        _lastChunkSize = _fileByteSize % _chunkSize; //in case there are some left overs 
        // 'numChunks' includes the last chunk.
        // If there was no remainder, then the last chunk is normal.
        // Make sure to set it, because it will be used on last iter:
        if(_lastChunkSize > 0){ _numChunks++; }
        else{ _lastChunkSize = _chunkSize; }

        fetchIntoBuff_thrd(true);
        _loadThread.join();

        fetchIntoBuff_thrd(false);//non-blocking (don't join())
        
        _isA = true;
        _readingChunk_id = 0;
    }


    void EndRead(){
        if(_loadThread.joinable()){  _loadThread.join();  }
        if(_file.is_open()){  _file.close(); }
    }


public:
    bool HasMoreForRead(){
        const bool isLastChunk = _readingChunk_id >= (_numChunks-1);
        return !isLastChunk  ||  !get_currBuff().endReached();
    }


    size_t currBuff_remainingBytes(){
        return get_currBuff().remaining();  
    }


    // Loads into buffers, and stores into 'outputHere'.
    // Swaps buffers until all information is retrieved.
    void read_rawData( char* outputHere, size_t numBytes ){
        assert(_file.is_open());

        while(numBytes > 0){
                RawData_Buff& buff =  get_currBuff();
                const size_t bufRemain =  buff.remaining();
                const size_t numCopy =  numBytes > bufRemain ?  bufRemain : numBytes;
        
                std::memcpy(outputHere, buff.data_current(), numCopy);
                buff.skipBytes(numCopy);

                if(buff.endReached()){
                    fetchIntoBuff_thrd( _isA );//start loading into the buffer that we used to read from.
                    focus_next_buffer();
                }
                outputHere += numCopy;
                numBytes -= numCopy;
        }//end while
    }


    template<typename T>
    void read_Literal(T& output){
        read_rawData((char*)&output, sizeof(T));
    }

    void read_String(std::string& output, size_t numChars){
        assert(_file.is_open());
        output.resize(numChars);
        read_rawData( output.data(), numChars );
    }


private:
    void fetchIntoBuff_thrd(bool isLoad_intoA){
        if (_loadThread.joinable()){ _loadThread.join(); }
        if (_readingChunk_id >= _numChunks){ return; }

        //NOTICE: not using [&] because 'isLoad_intoA' must be captured by VALUE only.
        //otherwise, when the scope ends, the value inside lambda will point to garbage.
        //so, both arguments are by value, but 'this' allows us to access the variables by reference
        //https://stackoverflow.com/a/21106201/9007125.
        auto lambda =  [isLoad_intoA, this]{
            //NOTICE: we don't use '_isA' because it might get changed while this thread works
            // (could be launched on a separate thread.)
            RawData_Buff& buf = isLoad_intoA ? _buff_a : _buff_b;

            //check if we will be loading a last chunk:
            bool isLastChunk = _readingChunk_id == (_numChunks - 1);
            size_t this_chunk_size = isLastChunk ? _lastChunkSize /* then fill chunk with remaining bytes */
                                                  : _chunkSize; /* else fill entire chunk */
            buf.reset_ix();
            _file.read((char*)buf.data_begin(), this_chunk_size);
            buf.set_apparent_size(this_chunk_size);
        };

        _loadThread = std::thread( lambda );
    }


private:
    RawData_Buff& get_currBuff(){  
        return _isA ? _buff_a : _buff_b;  
    }

    void focus_next_buffer(){
        if(HasMoreForRead()==false){ return; }
        _isA = !_isA;
        ++_readingChunk_id;
    }


private:
    std::ifstream _file;
    size_t _fileByteSize;
    size_t _numChunks;
    size_t _chunkSize;
    size_t _lastChunkSize;

    size_t _readingChunk_id=0;//which chunk are we 'reading' currently (no longer loading into it)

    bool _isA = true;
    RawData_Buff _buff_a;
    RawData_Buff _buff_b;

    std::thread _loadThread;
};