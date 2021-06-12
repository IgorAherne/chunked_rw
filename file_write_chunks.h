#pragma once
#include <string>
#include <fstream>
#include <filesystem>
#include <future>
#include <cassert>
#include "defines.h"

BEGIN_NAMESPACE(harvest)

// Add your bytes to the current buffer (there are two internally).
// When one buffer gets full it will be written to the file asynchronously, 
// while we continue filling the other buffer.
class file_writer_chunks {
public:
    // Choose the size that is likely to saturate HDD bandwidth.
    // Too little or too large will make you wait more than necessary, for HDD to complete.
    file_writer_chunks(){}


    ~file_writer_chunks(){
        if(_buff_A != nullptr){ delete[] _buff_A; }
        if(_buff_B != nullptr){ delete[] _buff_B; }
        _buff_A = _buff_B = nullptr;
    }

    // NOTICE: If file was created with re-reserved size, will show 
    // entire size of the file, including the reserved space:
    ssize_t fileSize_curr()const{
        if(_f.is_open()==false){ return -1; }
        return std::filesystem::file_size( _path_file_with_exten );
    }

    std::string filepath()const {  
        return _path_file_with_exten;  
    }


    void beingWrite( const std::string& path_file_with_exten,  
                     size_t startingFilesizeBytes = 1024,  
                     std::ios_base::openmode openMode = std::ios::trunc,
                     size_t bufferSizeBytes=1024*1024 ){
        //assert(bufferSizeBytes >= 1024);//else, not performant //MODIF
        _path_file_with_exten =  path_file_with_exten;
        _buffSizeBytes = bufferSizeBytes;
        _buff_A = new unsigned char[bufferSizeBytes];
        _buff_B = new unsigned char[bufferSizeBytes];

        if(_f.is_open()){ _f.close(); }
        if(std::filesystem::exists(path_file_with_exten)){
            _f.open(path_file_with_exten,  openMode | std::ios::binary);
        }else{
            _f.open(path_file_with_exten,  std::ios::binary );
        }
        if(!_f){  
            throw(std::runtime_error("file" + path_file_with_exten + "couldn't open")); 
        }

        std::filesystem::resize_file( path_file_with_exten, startingFilesizeBytes);
        _neverWroteToBuff_yet = true;
        _isA = true;
        _next_ix_inBuff = 0;
        _began = true;
    }


    // Very slow. If our buffers are currently being flushed, waits until they finished being flushed.
    // Then, blocks execution until complete and overwrites somewhere in the middle of the file
    void overwriteBytes_slow(size_t numBytesOffset_inFile,  const void* bytes,  size_t count){
        if(_writeTask_A.valid()){  _writeTask_A.get(); }
        if(_writeTask_B.valid()){  _writeTask_B.get(); }

        size_t p = _f.tellp();//NOTICE: location in file (p) is always in increments of '_buffSizeBytes'.

        // If we never stored anything to the buffers and
        // if file pointer is at start and
        // if overwriting with zero offset inside file:
        // then it is a special case, where we will just write the data into the buffer.
        // This is useful for files made of a "Header" and then a "Body".
        // Otherwise we would have a bug: we would successfully overwrite, reverting the file-pointer 
        // to its original place (to zero) ...but future buffers would flush over current stuff.
        if(_neverWroteToBuff_yet  &&  p == 0  &&  numBytesOffset_inFile == 0){
            writeBytes( bytes, count );
            return;
        }
        const bool completely_beforeArea =  numBytesOffset_inFile+count <= p;
        const bool completely_afterArea =  numBytesOffset_inFile >= p+_buffSizeBytes;

        if( !completely_beforeArea && !completely_afterArea ){
            // we want to alter part of file where the buffer will soon output.
            // For example, buffer might later output to a b c d and 4 more places,
            // but we want now to insert into x x x x x x x x 
            //
            //   _ _ _ _ _ _ _ _ _ a b c d x x x x x x x x _ _ _ _ _
            //
            // Therefore we need to first store to file whatever the buffer has so far.
            // Then, we can overwrite bytes where we want.
            // Flushing:
            flush_all_nonsaved_toFile();
            p = _f.tellp();//update p.
        }

        //you can only overwrite inside the file, or append to the end. Can't start far beyond:
        nn_dev_assert(numBytesOffset_inFile <= p);

        //NOTICE: we will overwrite any consecutive bytes in a file, NOT insert. http://www.cplusplus.com/forum/beginner/150097/
        _f.seekp(numBytesOffset_inFile, std::ios_base::beg);
        _f.write((const char*)bytes, count);
        _f.seekp(p, std::ios_base::beg);//come back to original place.
    }


    // Add bytes to one of two buffers.
    // The other buffer will be written to the file asynchronously, when becomes full.
    void writeBytes(const void* bytes,  size_t count){
        assert(_began);
        _neverWroteToBuff_yet = false;

        while( count > 0){
                if(_isA){
                //we will be soon writing to buffer A, so making sure it's no longer being written to file:
                if(_writeTask_A.valid()){  _writeTask_A.get(); }
                }else{//is B:
                    if(_writeTask_B.valid()){  _writeTask_B.get(); }
                }

                unsigned char* buff =  _isA ? _buff_A : _buff_B;//where we will write.
                const size_t numAvailabile =  _buffSizeBytes - _next_ix_inBuff;
                const size_t numToWrite =   count > numAvailabile ? numAvailabile : count;
            
                std::memcpy(buff + _next_ix_inBuff,  bytes,  numToWrite );
                
                if(numToWrite < numAvailabile){//"less than", NOT "less or equal".
                    _next_ix_inBuff += numToWrite;
                    return;
                }

                //flush the buffer into file:
                if(_isA){ _writeTask_A =  std::async(std::launch::async,  [=]{_f.write( (const char*)buff, _buffSizeBytes);} );  }
                else {    _writeTask_B =  std::async(std::launch::async,  [=]{_f.write( (const char*)buff, _buffSizeBytes);} );  }

                _isA = !_isA;
                _next_ix_inBuff = 0;
                bytes =  static_cast<const char*>(bytes) + numToWrite;
                count -= numToWrite;
        }//end while
    }



public:
    // Ensures that any remaining bytes get written to the file.
    // Blocks execution until complete
    void completeWrite(){
        assert(_began);
        flush_all_nonsaved_toFile();
        _f.close();//finish
        _path_file_with_exten = "";
        _began = false;
    }


private:
    void flush_all_nonsaved_toFile(){
        const size_t count =  _next_ix_inBuff;
        //else, some amount remains in one of the buffers:
        if(count > 0){
            if(_isA){
                //we will be soon writing to buffer A, so making sure it's no longer being written to file.
                if(_writeTask_A.valid()){  _writeTask_A.get();  }

                _writeTask_A =  std::async(std::launch::async,  [=]{_f.write((const char*)_buff_A, count); });
            }
            else{
                if(_writeTask_B.valid()){  _writeTask_B.get();  }

                _writeTask_B =  std::async(std::launch::async,  [=]{_f.write((const char*)_buff_B, count); });
            }//end if _isA==false
        }
        //ensure everything is completed before finishing this function.
        if(_writeTask_A.valid()){  _writeTask_A.get(); }
        if(_writeTask_B.valid()){  _writeTask_B.get(); }
        _next_ix_inBuff = 0;
        _isA = true;
    }


private:
    std::string _path_file_with_exten = "";
    std::ofstream _f;

    bool _began = false; //was beginWrite() called or not.

    bool _neverWroteToBuff_yet = true;
    unsigned char* _buff_A =nullptr;
    unsigned char* _buff_B =nullptr;
    size_t _buffSizeBytes = 0;

    bool _isA = true;
    size_t _next_ix_inBuff = 0;

    std::future<void> _writeTask_A;
    std::future<void> _writeTask_B;
};

END_NAMESPACE