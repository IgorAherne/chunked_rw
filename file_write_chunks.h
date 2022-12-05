// MIT LICENSE
// igor.aherne.business@gmail.com
// Requires C++17  for std::filesystem 

#pragma once
#include <string>
#include <fstream>
#include <filesystem>
#include <future>
#include <cassert>

// Add your bytes to the current buffer (there are two internally).
// When one buffer gets full it will be written to the file asynchronously, 
// while we continue filling the other buffer.
//
//  beingWrite()
//  completeWrite()
//  isOpen()
//  filepath()
//  fileSize_curr()
//  numBytesStored_soFar()
//  writeBytes()
//  overwriteBytes_slow()
//
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


    std::string filepath()const {
        std::lock_guard lck(_mu);
        if(_f.is_open()==false){ return ""; }
        return _path_file_with_exten;  
    }


    // NOTICE: If file was created with re-reserved size, will show 
    // entire size of the file, including the reserved space:
    ssize_t fileSize_curr()const{
        std::lock_guard lck(_mu);
        if(_f.is_open()==false){ return -1; }
        return std::filesystem::file_size( _path_file_with_exten );
    }
    

    //Caution: MIGHT NOT EQUAL TO CURRENT FILE SIZE. Use this to see how many bytes you've added.
    //This includes any bytes you might have overwritten in the middle of the file.
    size_t numBytesStored_soFar()const{
        return _numBytesStored;
    }


    bool isOpen()const{ 
        std::lock_guard lck(_mu); 
        return _f.is_open(); 
    }



    void beginWrite( const std::string& path_file_with_exten,  
                     size_t startingFilesizeBytes = 1024,  
                     std::ios_base::openmode openMode = std::ios::trunc,
                     size_t bufferSizeBytes=1024*1024 ){

        assert(bufferSizeBytes >= 1024);//else, not performant
        std::lock_guard lck(_mu);
        std::lock_guard lckFile(_mu_fileAccess);

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

            try {
                std::filesystem::resize_file( path_file_with_exten, startingFilesizeBytes);
            }catch(std::runtime_error err){
                auto myError = std::runtime_error("couldn't resize file " + path_file_with_exten 
                                            + " maybe check if there is enough disk space.");
                LogConsole::get().ErrorBad(myError.what());
                throw(myError);
            }
            _isA = true;
            _next_ix_inBuff = 0;
            _began = true;
    }


    
    // Ensures that any remaining bytes get written to the file.
    // Blocks execution until complete
    void completeWrite(){
        std::lock_guard lck(_mu);
        assert(_began);
        ensure_all_buffs_flushed_to_file();
            std::lock_guard lckFile(_mu_fileAccess);
                _f.close();//finish
                _path_file_with_exten = "";
                _began = false;
    }



    // Add bytes to one of two buffers.
    // The other buffer will be written to the file asynchronously, when becomes full.
    void writeBytes(const void* bytes,  size_t count){
        std::lock_guard lck(_mu);
            writeBytes_internal( bytes, count );
    }


    // Very slow. If our buffers are currently being flushed, waits until they finished being flushed.
    // Then, blocks execution until complete and overwrites somewhere in the middle of the file
    void overwriteBytes_slow(size_t numBytesOffset_inFile,  const void* bytes,  size_t count){
    
        std::lock_guard lck(_mu);
        
        ensure_all_buffs_flushed_to_file();

            std::lock_guard lckFile(_mu_fileAccess);

                size_t p = _f.tellp();
                bool fileEmpty_afterFlushAll =  p==0; //checks if the position remained at 0 even after flush-attempts of both buffers.

                //you can only overwrite inside the file, or append to the end. Can't start far beyond:
                nn_dev_assert(numBytesOffset_inFile <= p);

                //NOTICE: we will overwrite any consecutive bytes in a file, NOT insert. http://www.cplusplus.com/forum/beginner/150097/
                _f.seekp(numBytesOffset_inFile, std::ios_base::beg);
                _f.write((const char*)bytes, count);

                //NOTICE: both buffers were already flushed above. 

                if(fileEmpty_afterFlushAll){
                    /*Do nothing. 
                      That's because we wrote into the file for the first time, flush_all_nonsaved_toFile() didn't store anything.
                      So, keep the pointer where it ended up, DON'T revert it back (to zero).
                      Otherwise, some future buffer would dump itself into file at zero, overwriting our stuff*/
                }else{
                    _f.seekp(p, std::ios_base::beg);//come back to original place.
                }
    }


private:
    void ensure_all_buffs_flushed_to_file(){
        //NOTICE: mutex is already locked.

        if(_writeTask_A.valid()){  _writeTask_A.get();  }
        if(_writeTask_B.valid()){  _writeTask_B.get();  }

        const size_t count =  _next_ix_inBuff;

        if(count > 0){//if some amount remains in one of the buffers:
            if(_isA){  std::lock_guard lckFile(_mu_fileAccess); _f.write((const char*)_buff_A, count); } //_isA means we were gathering into A. Flush it now.
            else{      std::lock_guard lckFile(_mu_fileAccess); _f.write((const char*)_buff_B, count); }
        }
        _next_ix_inBuff = 0;
        _isA = true;
    }



    void writeBytes_internal(const void* bytes, size_t count){
        //NOTICE: mutex is already locked.
        assert(_began);//In case if you had exception inside beginWrite(), where there is no more space on hard-drive.

        _numBytesStored += count;  //ASSINGINING BEFORE the while(),  because count will bb decremented soon.

        while(count > 0){
                if(_isA){
                    //we wish to store into buffer A, so making sure it's no longer being written to file:
                    if(_writeTask_A.valid()){  _writeTask_A.get(); }
                }else{//we wish to store into B:
                    if(_writeTask_B.valid()){  _writeTask_B.get(); }
                }

                unsigned char* buff =  _isA ? _buff_A : _buff_B;//where we will store.
                const size_t numAvailabile =  _buffSizeBytes - _next_ix_inBuff;
                const size_t numToWrite =   count > numAvailabile ? numAvailabile : count;
            
                std::memcpy(buff + _next_ix_inBuff,  bytes,  numToWrite );
                _next_ix_inBuff += numToWrite;
                
                if(numToWrite < numAvailabile){ break; }//"less than", NOT "less or equal".

                //flush the buffer into file.  Notice, that we use [=] not [&]
                auto writingLambda = [=]{ 
                    std::lock_guard lckFile(this->_mu_fileAccess);
                    size_t pos = _f.tellp();
                    this->_f.write( (const char*)buff, _buffSizeBytes);
                };

                if(_isA){ _writeTask_A =  std::async(std::launch::async, writingLambda); }
                else {    _writeTask_B =  std::async(std::launch::async, writingLambda); }

                _isA = !_isA;
                _next_ix_inBuff = 0;
                bytes =  static_cast<const char*>(bytes) + numToWrite;
                count -= numToWrite;
        }//end while
    }


private:
    std::string _path_file_with_exten = "";
    std::ofstream _f;

    std::atomic_bool _began = false; //was beginWrite() called or not.

    size_t _buffSizeBytes = 0; //assigned once, during beginWrite().
    unsigned char* _buff_A =nullptr;
    unsigned char* _buff_B =nullptr;

    //which buffer are we storing into. Meanwhile, the other buffer might be getting saved to file:
    std::atomic_bool _isA = true; 
    std::atomic_size_t _next_ix_inBuff = 0;

    //Caution: MIGHT NOT EQUAL TO CURRENT FILE SIZE. Use this to see how many bytes you've added.
    //This includes any bytes you might have overwritten in the middle of the file.
    std::atomic<size_t> _numBytesStored = 0;

    std::future<void> _writeTask_A;
    std::future<void> _writeTask_B;

    mutable std::mutex _mu;//for user interacting with us
    mutable std::mutex _mu_fileAccess; //for cases when we are doing something with the _f variable.
};
