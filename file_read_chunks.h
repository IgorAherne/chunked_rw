// MIT LICENSE

#pragma once
#include <vector>
#include <fstream>
#include <string>
#include <filesystem>
#include <functional>
#include <thread>
#include "RawData_Buff.h"

namespace fs = std::filesystem;

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
        
        fs::path p(fileName_with_exten);
        _file.open(p, std::ios::binary);

        if (_file.is_open() == false){
            std::string message = std::string("file_read_chunks() could not open filePath: ") + fileName_with_exten;
            throw std::runtime_error(message);
            return;
        }
        
        _chunkSize =     _buff_a.totalAlocatedSize();
        _fileByteSize =  fs::file_size(p);//throws exception if path doesn't exist. 
        _numChunks =     (int)(_fileByteSize / _chunkSize);
        _lastChunkSize = _fileByteSize % _chunkSize; //in case there are some left overs 
        _ix_inEntireFile = 0;
        // 'numChunks' includes the last chunk.
        // If there was no remainder, then the last chunk is normal.
        // Make sure to set it, because it will be used on last iter:
        if(_lastChunkSize > 0){ _numChunks++; }
        else{ _lastChunkSize = _chunkSize; }

        bool willLoadIntoLastChunk = _numChunks==1;
        fetchIntoBuff_thrd(true, willLoadIntoLastChunk); // true: fill _buff_A (doesn't block the thread)
        
        if (_numChunks>1){
            willLoadIntoLastChunk = _numChunks==2;
            //at the start of the function it waits for the _buff_A to fill. (blocks the thread)
            fetchIntoBuff_thrd(false, willLoadIntoLastChunk);
        }else {
            if (_loadThread.joinable()){ _loadThread.join(); }//wait until _buff_A is filled
        }
        //NOTICE: don't invoke 'focus_next_buffer()' yet.

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

    
    size_t fileByteSize() const {  assert(_file.is_open()); return _fileByteSize;  }

    size_t remainingBytes_total() const { return _fileByteSize - _ix_inEntireFile; } //how many bytes we have left to read

    size_t remainingBytes_in_currBuff()const{ return get_currBuff().remaining(); }




    // Loads into buffers, and stores into 'outputHere'.
    // Swaps buffers until all information is retrieved.
    void read_rawData( char* outputHere, size_t numBytes ){
        assert(_file.is_open());
        if(numBytes > _fileByteSize-_ix_inEntireFile){ throw std::runtime_error("requesting more byte than there remains to be read."); }
        const size_t numBytes_copy = numBytes;

        while(numBytes > 0){
                RawData_Buff& buff =  get_currBuff();
                const size_t bufRemain =  buff.remaining();
                const size_t numCopy =  numBytes > bufRemain ?  bufRemain : numBytes;
        
                std::memcpy(outputHere, buff.data_current(), numCopy);
                buff.skipBytes(numCopy);

                if(buff.endReached()){
                    focus_next_buffer();
                    if(_readingChunk_id < _numChunks-1){   
                        //check if we are about to load into final chunk
                        int id_for_load =  _readingChunk_id+1;
                        bool willLoadIntoFinalChunk =  id_for_load == (_numChunks-1);
                        // NOTICE:  !_isA  because we start loading into the buffer 
                        // that we've just been using to read from.
                        fetchIntoBuff_thrd( !_isA, willLoadIntoFinalChunk);
                    }else{
                        // reading final chunk. MAke sure it was fully loaded. 
                        // ITS IMPORTANT!!! (the fetchIntoBuff_thrd() was synching, but we didn't run it in this 'else')
                        if (_loadThread.joinable()){ _loadThread.join(); }
                    }
                }
                outputHere += numCopy;
                numBytes -= numCopy;
        }//end while

        _ix_inEntireFile += numBytes_copy;
    }


    template<typename T>
    void read_Literal(T& output){
        read_rawData((char*)&output, sizeof(T));
    }

    void read_String(std::string& output, size_t numChars){
        assert(_file.is_open());
        output.resize(numChars);
        read_rawData( &output[0], numChars);
    }


private:
    void fetchIntoBuff_thrd(bool isLoad_intoA, bool isLoadIntoFinalChunk){
        if (_loadThread.joinable()){ _loadThread.join(); }

        size_t this_chunk_size =  isLoadIntoFinalChunk ? _lastChunkSize /* then fill chunk with remaining bytes */
                                                       : _chunkSize; /* else fill entire chunk */

        // NOTICE:  we don't use '_isA' because it might get changed while this thread works
        // (could be launched on a separate thread.)
        RawData_Buff* buf_ptr = isLoad_intoA ? &_buff_a : &_buff_b;
        
        //NOTICE: reset ix and set apparent size OUTSIDE of lambda, 
        //to avoid raise conditions when user invokes HasMoreForRead().
        buf_ptr->reset_ix();
        buf_ptr->set_apparent_size(this_chunk_size);

        if(this_chunk_size == 0){ return; }

        //NOTICE: not using [&] because 'isLoad_intoA' must be captured by VALUE only.
        //otherwise, when the scope ends, the value inside lambda will point to garbage.
        //so, both arguments are by value, but 'this' allows us to access the member vars by reference
        //https://stackoverflow.com/a/21106201/9007125.
        auto lambda =  [this_chunk_size, buf_ptr, this]{
            this->_file.read((char*)buf_ptr->data_begin(), this_chunk_size);
        };

        _loadThread = std::thread( lambda );
    }


private:
    const RawData_Buff& get_currBuff()const{  return _isA ? _buff_a : _buff_b;  }
          RawData_Buff& get_currBuff(){  return _isA ? _buff_a : _buff_b;  }

    void focus_next_buffer(){
        if(HasMoreForRead()==false){ return; }
        _isA = !_isA;
        ++_readingChunk_id;
    }


private:
    std::ifstream _file;
    size_t _fileByteSize = 0;
    size_t _ix_inEntireFile = 0;
    int _numChunks = 0;
    size_t _chunkSize = 0;
    size_t _lastChunkSize = 0;

    int _readingChunk_id=0;//which chunk are we 'reading' currently (no longer loading into it)

    bool _isA = true;
    RawData_Buff _buff_a;
    RawData_Buff _buff_b;

    std::thread _loadThread;
};
